#include "slsm_sst.h"

#include <errno.h>
#include <string.h>

_Static_assert(sizeof(struct slsm_sst_record) == 16, "unexpected SST record ABI");
_Static_assert(sizeof(struct slsm_sst_header) == 56, "unexpected SST header ABI");

void slsm_memtable_init(struct slsm_memtable *memtable)
{
    memset(memtable, 0, sizeof(*memtable));
}

int slsm_memtable_put(struct slsm_memtable *memtable, uint64_t key, uint64_t value)
{
    size_t index;

    if (memtable == NULL)
        return -EINVAL;
    for (index = 0; index < memtable->count; ++index) {
        if (memtable->records[index].key == key) {
            memtable->records[index].value = value;
            return 0;
        }
        if (memtable->records[index].key > key)
            break;
    }
    if (memtable->count == SLSM_SST_MAX_RECORDS)
        return -ENOSPC;
    if (index < memtable->count) {
        memmove(&memtable->records[index + 1], &memtable->records[index],
                (memtable->count - index) * sizeof(memtable->records[0]));
    }
    memtable->records[index] = (struct slsm_sst_record){.key = key, .value = value};
    ++memtable->count;
    return 0;
}

size_t slsm_sst_encoded_size(size_t record_count)
{
    if (record_count == 0 || record_count > SLSM_SST_MAX_RECORDS)
        return 0;
    return sizeof(struct slsm_sst_header) +
           record_count * sizeof(struct slsm_sst_record);
}

int slsm_sst_flush(const struct slsm_memtable *memtable, uint64_t sst_id,
                   uint64_t epoch, void *buffer, size_t capacity,
                   size_t *encoded_length, struct slsm_sst_metadata *metadata)
{
    struct slsm_sst_header header;
    size_t encoded_size;

    if (memtable == NULL || memtable->count == 0 || sst_id == 0 || epoch == 0 ||
        buffer == NULL || encoded_length == NULL || metadata == NULL)
        return -EINVAL;
    encoded_size = slsm_sst_encoded_size(memtable->count);
    if (encoded_size == 0 || capacity < encoded_size)
        return -ENOSPC;

    header = (struct slsm_sst_header){
        .magic = SLSM_SST_MAGIC,
        .version = SLSM_SST_VERSION,
        .record_count = (uint16_t)memtable->count,
        .sst_id = sst_id,
        .epoch = epoch,
        .min_key = memtable->records[0].key,
        .max_key = memtable->records[memtable->count - 1].key,
        .payload_bytes = memtable->count * sizeof(struct slsm_sst_record),
    };
    memcpy(buffer, &header, sizeof(header));
    memcpy((unsigned char *)buffer + sizeof(header), memtable->records,
           header.payload_bytes);
    *encoded_length = encoded_size;
    *metadata = (struct slsm_sst_metadata){
        .version = SLSM_METADATA_VERSION,
        .level = 0,
        .epoch = epoch,
        .min_key = header.min_key,
        .max_key = header.max_key,
    };
    return 0;
}

int slsm_sst_validate(const void *buffer, size_t length, uint64_t expected_sst_id,
                      uint64_t expected_epoch, struct slsm_sst_header *header)
{
    struct slsm_sst_header parsed;
    size_t expected_length;
    size_t index;
    struct slsm_sst_record previous = {0};

    if (buffer == NULL || length < sizeof(parsed))
        return -EINVAL;
    memcpy(&parsed, buffer, sizeof(parsed));
    if (parsed.magic != SLSM_SST_MAGIC || parsed.version != SLSM_SST_VERSION ||
        parsed.record_count == 0 || parsed.record_count > SLSM_SST_MAX_RECORDS ||
        parsed.sst_id == 0 || parsed.epoch == 0 || parsed.min_key > parsed.max_key ||
        parsed.payload_bytes !=
            (uint64_t)parsed.record_count * sizeof(struct slsm_sst_record))
        return -EBADMSG;
    expected_length = sizeof(parsed) + (size_t)parsed.payload_bytes;
    if (expected_length != length ||
        (expected_sst_id != 0 && parsed.sst_id != expected_sst_id) ||
        (expected_epoch != 0 && parsed.epoch != expected_epoch))
        return -EBADMSG;

    for (index = 0; index < parsed.record_count; ++index) {
        struct slsm_sst_record record;

        memcpy(&record, (const unsigned char *)buffer + sizeof(parsed) +
                          index * sizeof(record), sizeof(record));
        if ((index != 0 && record.key <= previous.key) ||
            record.key < parsed.min_key || record.key > parsed.max_key)
            return -EBADMSG;
        previous = record;
    }
    if (previous.key != parsed.max_key)
        return -EBADMSG;
    if (header != NULL)
        *header = parsed;
    return 0;
}

int slsm_sst_lookup(const void *buffer, size_t length, uint64_t key,
                    uint64_t *value)
{
    struct slsm_sst_header header;
    size_t low = 0;
    size_t high;

    if (value == NULL || slsm_sst_validate(buffer, length, 0, 0, &header) != 0)
        return -EBADMSG;
    if (key < header.min_key || key > header.max_key)
        return -ENOENT;
    high = header.record_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        struct slsm_sst_record record;

        memcpy(&record, (const unsigned char *)buffer + sizeof(header) +
                          middle * sizeof(record), sizeof(record));
        if (record.key == key) {
            *value = record.value;
            return 0;
        }
        if (record.key < key)
            low = middle + 1;
        else
            high = middle;
    }
    return -ENOENT;
}
