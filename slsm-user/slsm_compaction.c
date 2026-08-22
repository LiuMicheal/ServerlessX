#include "slsm_compaction.h"

#include <errno.h>
#include <string.h>

_Static_assert(sizeof(struct slsm_compaction_batch_header) == 88,
               "unexpected compaction header ABI");

static int validate_input(const uint8_t *batch, size_t batch_length,
                          const struct slsm_compaction_input *input,
                          uint64_t epoch, struct slsm_sst_header *sst_header)
{
    size_t offset;
    size_t length;

    if (input->offset > batch_length || input->length > batch_length - input->offset ||
        input->length > SIZE_MAX)
        return -EBADMSG;
    offset = (size_t)input->offset;
    length = (size_t)input->length;
    return slsm_sst_validate(batch + offset, length, input->sst_id, epoch,
                             sst_header);
}

int slsm_compaction_pack(const void *first, size_t first_length,
                         uint64_t first_sst_id, const void *second,
                         size_t second_length, uint64_t second_sst_id,
                         uint64_t job_id, uint64_t output_sst_id, uint64_t epoch,
                         void *buffer, size_t capacity, size_t *encoded_length)
{
    struct slsm_compaction_batch_header header;
    size_t total_length;

    if (first == NULL || second == NULL || buffer == NULL || encoded_length == NULL ||
        first_length == 0 || second_length == 0 || first_sst_id == 0 ||
        second_sst_id == 0 || output_sst_id == 0 || epoch == 0 || job_id == 0 ||
        first_sst_id == second_sst_id || first_sst_id == output_sst_id ||
        second_sst_id == output_sst_id || first_length > SLSM_MAX_SST_SIZE ||
        second_length > SLSM_MAX_SST_SIZE)
        return -EINVAL;
    if (slsm_sst_validate(first, first_length, first_sst_id, epoch, NULL) != 0 ||
        slsm_sst_validate(second, second_length, second_sst_id, epoch, NULL) != 0)
        return -EBADMSG;
    if (first_length > SIZE_MAX - sizeof(header) ||
        second_length > SIZE_MAX - sizeof(header) - first_length)
        return -EOVERFLOW;
    total_length = sizeof(header) + first_length + second_length;
    if (total_length > capacity || total_length > SLSM_MAX_SST_SIZE)
        return -ENOSPC;
    header = (struct slsm_compaction_batch_header){
        .magic = SLSM_COMPACTION_MAGIC,
        .version = SLSM_COMPACTION_VERSION,
        .input_count = SLSM_COMPACTION_INPUTS,
        .header_size = sizeof(header),
        .job_id = job_id,
        .output_sst_id = output_sst_id,
        .epoch = epoch,
        .inputs = {
            {sizeof(header), first_length, first_sst_id},
            {sizeof(header) + first_length, second_length, second_sst_id},
        },
    };
    memcpy(buffer, &header, sizeof(header));
    memcpy((uint8_t *)buffer + header.inputs[0].offset, first, first_length);
    memcpy((uint8_t *)buffer + header.inputs[1].offset, second, second_length);
    *encoded_length = total_length;
    return 0;
}

int slsm_compaction_validate(const void *buffer, size_t length,
                             struct slsm_compaction_batch_header *header)
{
    struct slsm_compaction_batch_header parsed;
    uint64_t end;
    unsigned int index;

    if (buffer == NULL || length < sizeof(parsed))
        return -EINVAL;
    memcpy(&parsed, buffer, sizeof(parsed));
    if (parsed.magic != SLSM_COMPACTION_MAGIC ||
        parsed.version != SLSM_COMPACTION_VERSION ||
        parsed.input_count != SLSM_COMPACTION_INPUTS ||
        parsed.header_size != sizeof(parsed) || parsed.job_id == 0 ||
        parsed.output_sst_id == 0 || parsed.epoch == 0)
        return -EBADMSG;
    end = parsed.header_size;
    for (index = 0; index < SLSM_COMPACTION_INPUTS; ++index) {
        const struct slsm_compaction_input *input = &parsed.inputs[index];
        uint64_t input_end;

        if (input->sst_id == 0 || input->sst_id == parsed.output_sst_id ||
            input->length == 0 || input->offset < parsed.header_size ||
            input->offset < end || input->offset > length ||
            input->length > length - input->offset)
            return -EBADMSG;
        input_end = input->offset + input->length;
        if (input_end < input->offset || input_end > length)
            return -EBADMSG;
        if (validate_input((const uint8_t *)buffer, length, input, parsed.epoch,
                           NULL) != 0)
            return -EBADMSG;
        end = input_end;
    }
    if (end != length || parsed.inputs[0].sst_id == parsed.inputs[1].sst_id)
        return -EBADMSG;
    if (header != NULL)
        *header = parsed;
    return 0;
}

static int collect_sst(const uint8_t *buffer, size_t length, uint64_t epoch,
                       struct slsm_memtable *memtable,
                       const struct slsm_sst_header *header)
{
    uint64_t key;

    /* Stage 1 SSTs are deliberately tiny; bound the scan for malformed ranges. */
    if (header->max_key - header->min_key > UINT64_C(4096))
        return -E2BIG;
    key = header->min_key;
    for (;;) {
        uint64_t value;
        int status = slsm_sst_lookup(buffer, length, key, &value);

        if (status == 0) {
            status = slsm_memtable_put(memtable, key, value);
            if (status != 0)
                return status;
        } else if (status != -ENOENT) {
            return status;
        }
        if (key == header->max_key || key == UINT64_MAX)
            break;
        ++key;
    }
    (void)epoch;
    return 0;
}

int slsm_compaction_merge(const void *buffer, size_t length, void *output,
                          size_t capacity, struct slsm_sst_metadata *metadata,
                          struct slsm_sst_header *output_header,
                          uint64_t *overlap_key, uint64_t *overlap_value)
{
    struct slsm_compaction_batch_header header;
    struct slsm_sst_header first_header;
    struct slsm_sst_header second_header;
    struct slsm_sst_header parsed_output;
    struct slsm_memtable memtable;
    struct slsm_sst_metadata output_metadata;
    size_t first_offset;
    size_t second_offset;
    size_t output_length;
    int status;

    if (output == NULL || metadata == NULL)
        return -EINVAL;
    status = slsm_compaction_validate(buffer, length, &header);
    if (status != 0)
        return status;
    first_offset = (size_t)header.inputs[0].offset;
    second_offset = (size_t)header.inputs[1].offset;
    status = validate_input(buffer, length, &header.inputs[0], header.epoch,
                            &first_header);
    if (status == 0)
        status = validate_input(buffer, length, &header.inputs[1], header.epoch,
                                &second_header);
    if (status != 0)
        return -EBADMSG;
    slsm_memtable_init(&memtable);
    status = collect_sst((const uint8_t *)buffer + first_offset,
                         (size_t)header.inputs[0].length, header.epoch, &memtable,
                         &first_header);
    if (status == 0)
        status = collect_sst((const uint8_t *)buffer + second_offset,
                             (size_t)header.inputs[1].length, header.epoch, &memtable,
                             &second_header);
    if (status != 0)
        return status;
    status = slsm_sst_flush(&memtable, header.output_sst_id, header.epoch, output,
                            capacity, &output_length, &output_metadata);
    if (status != 0)
        return status;
    /* Flush produces an L0 table; this output is the result of an L0 merge. */
    output_metadata.level = 1;
    memcpy(&parsed_output, output, sizeof(parsed_output));
    if (output_header != NULL)
        *output_header = parsed_output;
    *metadata = output_metadata;
    if (overlap_key != NULL && overlap_value != NULL) {
        uint64_t key;
        int found = 0;

        *overlap_key = 0;
        *overlap_value = 0;
        key = first_header.min_key > second_header.min_key ? first_header.min_key
                                                            : second_header.min_key;
        while (key <= first_header.max_key && key <= second_header.max_key) {
            uint64_t first_value;
            uint64_t second_value;

            if (slsm_sst_lookup((const uint8_t *)buffer + first_offset,
                                (size_t)header.inputs[0].length, key,
                                &first_value) == 0 &&
                slsm_sst_lookup((const uint8_t *)buffer + second_offset,
                                (size_t)header.inputs[1].length, key,
                                &second_value) == 0) {
                *overlap_key = key;
                *overlap_value = second_value;
                found = 1;
                break;
            }
            if (key == UINT64_MAX)
                break;
            ++key;
        }
        if (!found)
            return -EBADMSG;
    }
    (void)output_length;
    return 0;
}
