#include "slsm_sst.h"

#include "slsm_nova_sst.h"

#include <errno.h>
#include <string.h>

_Static_assert(sizeof(struct slsm_sst_record) == 16, "unexpected SST record ABI");
_Static_assert(sizeof(struct slsm_sst_header) == 56, "unexpected SST header ABI");

static void encode_numeric_key(uint8_t output[sizeof(uint64_t)], uint64_t key)
{
    size_t index;

    /* Nova's bytewise comparator then preserves the numeric MemTable order. */
    for (index = 0; index < sizeof(key); ++index)
        output[index] = (uint8_t)(key >> ((sizeof(key) - index - 1) * 8));
}

static void encode_value(uint8_t output[sizeof(uint64_t)], uint64_t value)
{
    size_t index;

    for (index = 0; index < sizeof(value); ++index)
        output[index] = (uint8_t)(value >> (index * 8));
}

static uint64_t decode_value(const uint8_t input[sizeof(uint64_t)])
{
    uint64_t value = 0;
    size_t index;

    for (index = 0; index < sizeof(value); ++index)
        value |= (uint64_t)input[index] << (index * 8);
    return value;
}

void slsm_memtable_init(struct slsm_memtable *memtable)
{
    if (memtable != NULL)
        memset(memtable, 0, sizeof(*memtable));
}

int slsm_memtable_put(struct slsm_memtable *memtable, uint64_t key, uint64_t value)
{
    size_t index;

    if (memtable == NULL || memtable->count > SLSM_SST_MAX_RECORDS)
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
    return sizeof(struct slsm_sst_header) + SLSM_NOVA_TABLE_MAX_SIZE;
}

int slsm_sst_flush(const struct slsm_memtable *memtable, uint64_t sst_id,
                   uint64_t epoch, void *buffer, size_t capacity,
                   size_t *encoded_length, struct slsm_sst_metadata *metadata)
{
    struct slsm_nova_entry entries[SLSM_SST_MAX_RECORDS];
    uint8_t keys[SLSM_SST_MAX_RECORDS][sizeof(uint64_t)];
    uint8_t values[SLSM_SST_MAX_RECORDS][sizeof(uint64_t)];
    struct slsm_nova_table_info table_info;
    struct slsm_sst_header header;
    size_t payload_length;
    size_t index;
    int status;

    if (memtable == NULL || memtable->count == 0 ||
        memtable->count > SLSM_SST_MAX_RECORDS || sst_id == 0 || epoch == 0 ||
        buffer == NULL || encoded_length == NULL || metadata == NULL ||
        capacity < sizeof(header))
        return -EINVAL;
    for (index = 0; index < memtable->count; ++index) {
        encode_numeric_key(keys[index], memtable->records[index].key);
        encode_value(values[index], memtable->records[index].value);
        entries[index] = (struct slsm_nova_entry){
            .user_key = keys[index],
            .user_key_length = sizeof(keys[index]),
            .value = values[index],
            .value_length = sizeof(values[index]),
            .sequence = SLSM_NOVA_MAX_SEQUENCE,
            .type = SLSM_NOVA_TYPE_VALUE,
        };
    }
    status = slsm_nova_table_build(entries, memtable->count,
                                   (uint8_t *)buffer + sizeof(header),
                                   capacity - sizeof(header), &payload_length,
                                   &table_info);
    if (status != 0)
        return status;
    header = (struct slsm_sst_header){
        .magic = SLSM_SST_MAGIC,
        .version = SLSM_SST_VERSION,
        .record_count = (uint16_t)memtable->count,
        .sst_id = sst_id,
        .epoch = epoch,
        .min_key = memtable->records[0].key,
        .max_key = memtable->records[memtable->count - 1].key,
        .payload_bytes = payload_length,
    };
    memcpy(buffer, &header, sizeof(header));
    *encoded_length = sizeof(header) + payload_length;
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
    struct slsm_nova_table_info table_info;
    int status;

    if (buffer == NULL || length < sizeof(parsed))
        return -EINVAL;
    memcpy(&parsed, buffer, sizeof(parsed));
    if (parsed.magic != SLSM_SST_MAGIC || parsed.version != SLSM_SST_VERSION ||
        parsed.record_count == 0 || parsed.record_count > SLSM_SST_MAX_RECORDS ||
        parsed.sst_id == 0 || parsed.epoch == 0 || parsed.min_key > parsed.max_key ||
        parsed.payload_bytes < SLSM_NOVA_FOOTER_SIZE ||
        parsed.payload_bytes > SIZE_MAX - sizeof(parsed) ||
        sizeof(parsed) + (size_t)parsed.payload_bytes != length ||
        (expected_sst_id != 0 && parsed.sst_id != expected_sst_id) ||
        (expected_epoch != 0 && parsed.epoch != expected_epoch))
        return -EBADMSG;
    status = slsm_nova_table_validate((const uint8_t *)buffer + sizeof(parsed),
                                      (size_t)parsed.payload_bytes, &table_info);
    if (status != 0 || table_info.entry_count != parsed.record_count)
        return -EBADMSG;
    if (header != NULL)
        *header = parsed;
    return 0;
}

int slsm_sst_lookup(const void *buffer, size_t length, uint64_t key,
                    uint64_t *value)
{
    struct slsm_sst_header header;
    uint8_t encoded_key[sizeof(uint64_t)];
    const uint8_t *encoded_value;
    size_t value_length;
    uint8_t value_type;
    uint64_t sequence;
    int status;

    if (value == NULL || slsm_sst_validate(buffer, length, 0, 0, &header) != 0)
        return -EBADMSG;
    if (key < header.min_key || key > header.max_key)
        return -ENOENT;
    encode_numeric_key(encoded_key, key);
    status = slsm_nova_table_lookup(
        (const uint8_t *)buffer + sizeof(header), (size_t)header.payload_bytes,
        encoded_key, sizeof(encoded_key), SLSM_NOVA_MAX_SEQUENCE,
        &encoded_value, &value_length, &value_type, &sequence);
    if (status != 0)
        return status;
    if (value_type != SLSM_NOVA_TYPE_VALUE || value_length != sizeof(uint64_t) ||
        sequence == 0)
        return -EBADMSG;
    *value = decode_value(encoded_value);
    return 0;
}
