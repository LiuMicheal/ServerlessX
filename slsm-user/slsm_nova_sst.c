#include "slsm_nova_sst.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define NOVA_BLOCK_HANDLE_MAX_SIZE 20U
#define NOVA_COMPRESSION_NONE 0U
#define NOVA_CRC_MASK_DELTA UINT32_C(0xa282ead8)

struct byte_writer {
    uint8_t *data;
    size_t capacity;
    size_t length;
};

struct nova_block_handle {
    uint64_t offset;
    uint64_t size;
};

struct nova_block_view {
    const uint8_t *data;
    size_t length;
    size_t entries_end;
    size_t restart_count;
};

struct nova_footer {
    struct nova_block_handle metaindex;
    struct nova_block_handle index;
};

struct nova_block_entry {
    const uint8_t *value;
    size_t value_length;
    size_t next_offset;
};

struct nova_table_layout {
    struct nova_block_handle data;
    struct nova_block_handle metaindex;
    struct nova_block_handle index;
};

static uint32_t decode_fixed32(const uint8_t *input)
{
    return (uint32_t)input[0] | ((uint32_t)input[1] << 8) |
           ((uint32_t)input[2] << 16) | ((uint32_t)input[3] << 24);
}

static uint64_t decode_fixed64(const uint8_t *input)
{
    return (uint64_t)decode_fixed32(input) |
           ((uint64_t)decode_fixed32(input + 4) << 32);
}

static void encode_fixed32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
    output[2] = (uint8_t)(value >> 16);
    output[3] = (uint8_t)(value >> 24);
}

static void encode_fixed64(uint8_t *output, uint64_t value)
{
    encode_fixed32(output, (uint32_t)value);
    encode_fixed32(output + 4, (uint32_t)(value >> 32));
}

static int writer_append(struct byte_writer *writer, const void *data, size_t length)
{
    if (writer == NULL || (data == NULL && length != 0) ||
        length > writer->capacity - writer->length)
        return -ENOSPC;
    if (length != 0)
        memcpy(writer->data + writer->length, data, length);
    writer->length += length;
    return 0;
}

static int writer_append_fixed32(struct byte_writer *writer, uint32_t value)
{
    uint8_t encoded[4];

    encode_fixed32(encoded, value);
    return writer_append(writer, encoded, sizeof(encoded));
}

static int writer_append_fixed64(struct byte_writer *writer, uint64_t value)
{
    uint8_t encoded[8];

    encode_fixed64(encoded, value);
    return writer_append(writer, encoded, sizeof(encoded));
}

static int writer_append_varint(struct byte_writer *writer, uint64_t value)
{
    uint8_t encoded[10];
    size_t length = 0;

    do {
        uint8_t byte = (uint8_t)(value & UINT64_C(0x7f));

        value >>= 7;
        if (value != 0)
            byte |= UINT8_C(0x80);
        encoded[length++] = byte;
    } while (value != 0);
    return writer_append(writer, encoded, length);
}

static int decode_varint(const uint8_t *input, size_t length, size_t *offset,
                         uint64_t *value)
{
    uint64_t result = 0;
    unsigned int shift;
    size_t cursor;

    if (input == NULL || offset == NULL || value == NULL || *offset > length)
        return -EBADMSG;
    cursor = *offset;
    for (shift = 0; shift <= 63 && cursor < length; shift += 7) {
        uint8_t byte = input[cursor++];

        if (shift == 63 && (byte & UINT8_C(0xfe)) != 0)
            return -EBADMSG;
        result |= (uint64_t)(byte & UINT8_C(0x7f)) << shift;
        if ((byte & UINT8_C(0x80)) == 0) {
            *offset = cursor;
            *value = result;
            return 0;
        }
    }
    return -EBADMSG;
}

static int compare_bytes(const uint8_t *left, size_t left_length,
                         const uint8_t *right, size_t right_length)
{
    size_t common = left_length < right_length ? left_length : right_length;
    int result = common == 0 ? 0 : memcmp(left, right, common);

    if (result != 0)
        return result < 0 ? -1 : 1;
    if (left_length == right_length)
        return 0;
    return left_length < right_length ? -1 : 1;
}

int slsm_nova_internal_key_encode(const uint8_t *user_key, size_t user_key_length,
                                  uint64_t sequence, uint8_t type,
                                  uint8_t *output, size_t capacity,
                                  size_t *encoded_length)
{
    uint64_t tag;

    if ((user_key == NULL && user_key_length != 0) || output == NULL ||
        encoded_length == NULL || sequence > SLSM_NOVA_MAX_SEQUENCE ||
        type > SLSM_NOVA_TYPE_VALUE || user_key_length > SIZE_MAX - 8)
        return -EINVAL;
    if (capacity < user_key_length + 8)
        return -ENOSPC;
    if (user_key_length != 0)
        memcpy(output, user_key, user_key_length);
    tag = (sequence << 8) | type;
    encode_fixed64(output + user_key_length, tag);
    *encoded_length = user_key_length + 8;
    return 0;
}

int slsm_nova_internal_key_decode(const uint8_t *internal_key,
                                  size_t internal_key_length,
                                  const uint8_t **user_key,
                                  size_t *user_key_length, uint64_t *sequence,
                                  uint8_t *type)
{
    uint64_t tag;
    uint8_t parsed_type;

    if (internal_key == NULL || internal_key_length < 8 || user_key == NULL ||
        user_key_length == NULL || sequence == NULL || type == NULL)
        return -EINVAL;
    tag = decode_fixed64(internal_key + internal_key_length - 8);
    parsed_type = (uint8_t)tag;
    if (parsed_type > SLSM_NOVA_TYPE_VALUE)
        return -EBADMSG;
    *user_key = internal_key;
    *user_key_length = internal_key_length - 8;
    *sequence = tag >> 8;
    *type = parsed_type;
    return 0;
}

int slsm_nova_internal_key_compare(const uint8_t *left, size_t left_length,
                                   const uint8_t *right, size_t right_length)
{
    const uint8_t *left_user;
    const uint8_t *right_user;
    size_t left_user_length;
    size_t right_user_length;
    uint64_t left_sequence;
    uint64_t right_sequence;
    uint8_t left_type;
    uint8_t right_type;
    int result;

    if (slsm_nova_internal_key_decode(left, left_length, &left_user,
                                      &left_user_length, &left_sequence,
                                      &left_type) != 0 ||
        slsm_nova_internal_key_decode(right, right_length, &right_user,
                                      &right_user_length, &right_sequence,
                                      &right_type) != 0)
        return 0;
    result = compare_bytes(left_user, left_user_length, right_user,
                           right_user_length);
    if (result != 0)
        return result;
    if (left_sequence != right_sequence)
        return left_sequence > right_sequence ? -1 : 1;
    if (left_type == right_type)
        return 0;
    return left_type > right_type ? -1 : 1;
}

static uint32_t crc32c_extend(uint32_t crc, const uint8_t *data, size_t length)
{
    size_t index;

    crc = ~crc;
    for (index = 0; index < length; ++index) {
        unsigned int bit;

        crc ^= data[index];
        for (bit = 0; bit < 8; ++bit) {
            uint32_t mask = UINT32_C(0) - (crc & UINT32_C(1));

            crc = (crc >> 1) ^ (UINT32_C(0x82f63b78) & mask);
        }
    }
    return ~crc;
}

static uint32_t crc32c_mask(uint32_t crc)
{
    return ((crc >> 15) | (crc << 17)) + NOVA_CRC_MASK_DELTA;
}

static int append_block_trailer(struct byte_writer *table, size_t block_offset,
                                size_t block_length)
{
    uint8_t trailer[SLSM_NOVA_BLOCK_TRAILER_SIZE];
    uint32_t crc;

    trailer[0] = NOVA_COMPRESSION_NONE;
    crc = crc32c_extend(0, table->data + block_offset, block_length);
    crc = crc32c_extend(crc, trailer, 1);
    encode_fixed32(trailer + 1, crc32c_mask(crc));
    return writer_append(table, trailer, sizeof(trailer));
}

static int validate_block_trailer(const uint8_t *table, size_t table_length,
                                  const struct nova_block_handle *handle)
{
    const uint8_t *trailer;
    uint32_t crc;

    if (handle->offset > table_length || handle->size > table_length - handle->offset ||
        SLSM_NOVA_BLOCK_TRAILER_SIZE >
            table_length - handle->offset - handle->size)
        return -EBADMSG;
    trailer = table + (size_t)handle->offset + (size_t)handle->size;
    if (trailer[0] != NOVA_COMPRESSION_NONE)
        return -ENOTSUP;
    crc = crc32c_extend(0, table + (size_t)handle->offset, (size_t)handle->size);
    crc = crc32c_extend(crc, trailer, 1);
    if (decode_fixed32(trailer + 1) != crc32c_mask(crc))
        return -EBADMSG;
    return 0;
}

static int append_block_handle(struct byte_writer *writer,
                               const struct nova_block_handle *handle)
{
    int status;

    status = writer_append_varint(writer, handle->offset);
    if (status == 0)
        status = writer_append_varint(writer, handle->size);
    return status;
}

static int decode_block_handle(const uint8_t *input, size_t length, size_t *offset,
                               struct nova_block_handle *handle)
{
    int status;

    status = decode_varint(input, length, offset, &handle->offset);
    if (status == 0)
        status = decode_varint(input, length, offset, &handle->size);
    return status;
}

static int append_block_entry(struct byte_writer *block, const uint8_t *previous_key,
                              size_t previous_key_length, const uint8_t *key,
                              size_t key_length, const uint8_t *value,
                              size_t value_length, size_t entry_index,
                              uint32_t *restarts, size_t *restart_count)
{
    size_t shared = 0;
    int status;

    if (entry_index % SLSM_NOVA_RESTART_INTERVAL == 0) {
        if (block->length > UINT32_MAX)
            return -EOVERFLOW;
        restarts[(*restart_count)++] = (uint32_t)block->length;
    } else {
        size_t common = previous_key_length < key_length ? previous_key_length : key_length;

        while (shared < common && previous_key[shared] == key[shared])
            ++shared;
    }
    status = writer_append_varint(block, shared);
    if (status == 0)
        status = writer_append_varint(block, key_length - shared);
    if (status == 0)
        status = writer_append_varint(block, value_length);
    if (status == 0)
        status = writer_append(block, key + shared, key_length - shared);
    if (status == 0)
        status = writer_append(block, value, value_length);
    return status;
}

static int finish_block(struct byte_writer *block, const uint32_t *restarts,
                        size_t restart_count)
{
    size_t index;
    int status = 0;

    if (restart_count == 0 || restart_count > UINT32_MAX)
        return -EINVAL;
    for (index = 0; index < restart_count && status == 0; ++index)
        status = writer_append_fixed32(block, restarts[index]);
    if (status == 0)
        status = writer_append_fixed32(block, (uint32_t)restart_count);
    return status;
}

static int build_empty_block(uint8_t *storage, size_t capacity, size_t *length)
{
    struct byte_writer block = {.data = storage, .capacity = capacity};
    uint32_t restart = 0;
    int status = finish_block(&block, &restart, 1);

    if (status == 0)
        *length = block.length;
    return status;
}

static int append_raw_block(struct byte_writer *table, const uint8_t *block,
                            size_t block_length, struct nova_block_handle *handle)
{
    int status;

    handle->offset = table->length;
    handle->size = block_length;
    status = writer_append(table, block, block_length);
    if (status == 0)
        status = append_block_trailer(table, (size_t)handle->offset, block_length);
    return status;
}

static int append_footer(struct byte_writer *table,
                         const struct nova_block_handle *metaindex,
                         const struct nova_block_handle *index)
{
    uint8_t footer[SLSM_NOVA_FOOTER_SIZE] = {0};
    struct byte_writer writer = {.data = footer, .capacity = sizeof(footer)};
    int status;

    status = append_block_handle(&writer, metaindex);
    if (status == 0)
        status = append_block_handle(&writer, index);
    if (status != 0 || writer.length > SLSM_NOVA_FOOTER_SIZE - 8)
        return status != 0 ? status : -EOVERFLOW;
    writer.length = SLSM_NOVA_FOOTER_SIZE - 8;
    status = writer_append_fixed64(&writer, SLSM_NOVA_TABLE_MAGIC);
    if (status == 0)
        status = writer_append(table, footer, sizeof(footer));
    return status;
}

int slsm_nova_table_build(const struct slsm_nova_entry *entries, size_t entry_count,
                          void *buffer, size_t capacity, size_t *encoded_length,
                          struct slsm_nova_table_info *info)
{
    struct byte_writer table = {.data = buffer, .capacity = capacity};
    uint8_t *data_block = NULL;
    uint8_t *index_block = NULL;
    uint8_t empty_block[8];
    uint32_t *restarts = NULL;
    struct nova_block_handle data_handle;
    struct nova_block_handle metaindex_handle;
    struct nova_block_handle index_handle;
    struct byte_writer data_writer;
    struct byte_writer index_writer;
    size_t restart_count = 0;
    size_t empty_length = 0;
    uint8_t previous_key[UINT16_MAX + 8U];
    size_t previous_key_length = 0;
    uint8_t last_key[UINT16_MAX + 8U];
    size_t last_key_length = 0;
    size_t entry_index;
    int status = 0;

    if (entries == NULL || entry_count == 0 || buffer == NULL ||
        encoded_length == NULL || capacity < SLSM_NOVA_FOOTER_SIZE ||
        entry_count > UINT32_MAX)
        return -EINVAL;
    data_block = malloc(capacity);
    index_block = malloc(capacity);
    restarts = malloc(entry_count * sizeof(*restarts));
    if (data_block == NULL || index_block == NULL || restarts == NULL) {
        status = -ENOMEM;
        goto out;
    }
    data_writer = (struct byte_writer){.data = data_block, .capacity = capacity};
    index_writer = (struct byte_writer){.data = index_block, .capacity = capacity};

    for (entry_index = 0; entry_index < entry_count; ++entry_index) {
        const struct slsm_nova_entry *entry = &entries[entry_index];
        size_t internal_key_length;

        if ((entry->user_key == NULL && entry->user_key_length != 0) ||
            (entry->value == NULL && entry->value_length != 0) ||
            entry->user_key_length > UINT16_MAX) {
            status = -EINVAL;
            goto out;
        }
        status = slsm_nova_internal_key_encode(
            entry->user_key, entry->user_key_length, entry->sequence, entry->type,
            last_key, sizeof(last_key), &internal_key_length);
        if (status != 0)
            goto out;
        if (entry_index != 0 &&
            slsm_nova_internal_key_compare(previous_key, previous_key_length,
                                           last_key, internal_key_length) >= 0) {
            status = -EINVAL;
            goto out;
        }
        status = append_block_entry(&data_writer, previous_key, previous_key_length,
                                    last_key, internal_key_length, entry->value,
                                    entry->value_length, entry_index, restarts,
                                    &restart_count);
        if (status != 0)
            goto out;
        memcpy(previous_key, last_key, internal_key_length);
        previous_key_length = internal_key_length;
        last_key_length = internal_key_length;
    }
    status = finish_block(&data_writer, restarts, restart_count);
    if (status == 0)
        status = append_raw_block(&table, data_block, data_writer.length,
                                  &data_handle);
    if (status == 0)
        status = build_empty_block(empty_block, sizeof(empty_block), &empty_length);
    if (status == 0)
        status = append_raw_block(&table, empty_block, empty_length,
                                  &metaindex_handle);
    if (status == 0) {
        uint8_t handle_encoding[NOVA_BLOCK_HANDLE_MAX_SIZE];
        struct byte_writer handle_writer = {
            .data = handle_encoding,
            .capacity = sizeof(handle_encoding),
        };
        uint32_t index_restart = 0;
        size_t index_restart_count = 0;

        status = append_block_handle(&handle_writer, &data_handle);
        if (status == 0)
            status = append_block_entry(&index_writer, NULL, 0, last_key,
                                        last_key_length, handle_encoding,
                                        handle_writer.length, 0, &index_restart,
                                        &index_restart_count);
        if (status == 0)
            status = finish_block(&index_writer, &index_restart,
                                  index_restart_count);
    }
    if (status == 0)
        status = append_raw_block(&table, index_block, index_writer.length,
                                  &index_handle);
    if (status == 0)
        status = append_footer(&table, &metaindex_handle, &index_handle);
    if (status == 0) {
        *encoded_length = table.length;
        if (info != NULL)
            *info = (struct slsm_nova_table_info){
                .entry_count = entry_count,
                .data_block_count = 1,
            };
    }

out:
    free(restarts);
    free(index_block);
    free(data_block);
    return status;
}

static int parse_footer(const uint8_t *table, size_t length,
                        struct nova_footer *footer)
{
    const uint8_t *encoded;
    size_t offset = 0;
    int status;

    if (table == NULL || footer == NULL || length < SLSM_NOVA_FOOTER_SIZE)
        return -EBADMSG;
    encoded = table + length - SLSM_NOVA_FOOTER_SIZE;
    if (decode_fixed64(encoded + SLSM_NOVA_FOOTER_SIZE - 8) !=
        SLSM_NOVA_TABLE_MAGIC)
        return -EBADMSG;
    status = decode_block_handle(encoded, SLSM_NOVA_FOOTER_SIZE - 8, &offset,
                                 &footer->metaindex);
    if (status == 0)
        status = decode_block_handle(encoded, SLSM_NOVA_FOOTER_SIZE - 8, &offset,
                                     &footer->index);
    return status;
}

static int open_block(const uint8_t *table, size_t table_length,
                      const struct nova_block_handle *handle,
                      struct nova_block_view *block)
{
    uint32_t restart_count;
    size_t restart_bytes;
    size_t restart_index;
    uint32_t previous = 0;
    int status;

    status = validate_block_trailer(table, table_length, handle);
    if (status != 0)
        return status;
    if (handle->size < 8 || handle->size > SIZE_MAX)
        return -EBADMSG;
    block->data = table + (size_t)handle->offset;
    block->length = (size_t)handle->size;
    restart_count = decode_fixed32(block->data + block->length - 4);
    if (restart_count == 0 || restart_count > (block->length - 4) / 4)
        return -EBADMSG;
    restart_bytes = ((size_t)restart_count + 1) * 4;
    block->entries_end = block->length - restart_bytes;
    block->restart_count = restart_count;
    for (restart_index = 0; restart_index < restart_count; ++restart_index) {
        uint32_t current = decode_fixed32(block->data + block->entries_end +
                                          restart_index * 4);

        if (current > block->entries_end ||
            (restart_index != 0 && current <= previous))
            return -EBADMSG;
        previous = current;
    }
    if (decode_fixed32(block->data + block->entries_end) != 0)
        return -EBADMSG;
    return 0;
}

static int decode_block_entry(const struct nova_block_view *block, size_t offset,
                              uint8_t *key, size_t key_capacity,
                              size_t *key_length, struct nova_block_entry *entry)
{
    uint64_t shared;
    uint64_t unshared;
    uint64_t value_length;
    size_t cursor = offset;
    int status;

    if (block == NULL || key == NULL || key_length == NULL || entry == NULL ||
        offset >= block->entries_end)
        return -EBADMSG;
    status = decode_varint(block->data, block->entries_end, &cursor, &shared);
    if (status == 0)
        status = decode_varint(block->data, block->entries_end, &cursor, &unshared);
    if (status == 0)
        status = decode_varint(block->data, block->entries_end, &cursor,
                               &value_length);
    if (status != 0 || shared > *key_length || unshared > key_capacity - shared ||
        unshared > block->entries_end - cursor)
        return -EBADMSG;
    memcpy(key + (size_t)shared, block->data + cursor, (size_t)unshared);
    cursor += (size_t)unshared;
    if (value_length > block->entries_end - cursor)
        return -EBADMSG;
    *key_length = (size_t)(shared + unshared);
    entry->value = block->data + cursor;
    entry->value_length = (size_t)value_length;
    entry->next_offset = cursor + (size_t)value_length;
    return 0;
}

static int parse_index(const uint8_t *table, size_t table_length,
                       const struct nova_block_handle *index_handle,
                       struct nova_block_handle *data_handle)
{
    struct nova_block_view index;
    struct nova_block_entry entry;
    uint8_t key[UINT16_MAX + 8U];
    size_t key_length = 0;
    size_t handle_offset = 0;
    int status;

    status = open_block(table, table_length, index_handle, &index);
    if (status != 0)
        return status;
    status = decode_block_entry(&index, 0, key, sizeof(key), &key_length, &entry);
    if (status != 0 || entry.next_offset != index.entries_end)
        return -EBADMSG;
    status = decode_block_handle(entry.value, entry.value_length, &handle_offset,
                                 data_handle);
    if (status != 0 || handle_offset != entry.value_length)
        return -EBADMSG;
    return 0;
}

static int parse_table_layout(const uint8_t *table, size_t length,
                              struct nova_table_layout *layout)
{
    struct nova_footer footer;
    struct nova_block_view metaindex;
    size_t footer_offset;
    int status;

    status = parse_footer(table, length, &footer);
    if (status != 0)
        return status;
    footer_offset = length - SLSM_NOVA_FOOTER_SIZE;
    status = open_block(table, length, &footer.metaindex, &metaindex);
    if (status != 0 || metaindex.entries_end != 0)
        return status != 0 ? status : -EBADMSG;
    status = parse_index(table, length, &footer.index, &layout->data);
    if (status != 0)
        return status;
    if (layout->data.offset != 0 ||
        layout->data.size + SLSM_NOVA_BLOCK_TRAILER_SIZE != footer.metaindex.offset ||
        footer.metaindex.offset + footer.metaindex.size +
                SLSM_NOVA_BLOCK_TRAILER_SIZE != footer.index.offset ||
        footer.index.offset + footer.index.size + SLSM_NOVA_BLOCK_TRAILER_SIZE !=
            footer_offset)
        return -EBADMSG;
    layout->metaindex = footer.metaindex;
    layout->index = footer.index;
    return 0;
}

static int scan_data_block(const struct nova_block_view *block,
                           const uint8_t *lookup_user_key,
                           size_t lookup_user_key_length, uint64_t snapshot,
                           const uint8_t **value, size_t *value_length,
                           uint8_t *value_type, uint64_t *sequence,
                           size_t *entry_count)
{
    uint8_t key[UINT16_MAX + 8U];
    uint8_t previous_key[UINT16_MAX + 8U];
    size_t key_length = 0;
    size_t previous_key_length = 0;
    size_t offset = 0;
    size_t count = 0;
    int found = 0;

    while (offset < block->entries_end) {
        struct nova_block_entry entry;
        const uint8_t *user_key;
        size_t user_key_length;
        uint64_t parsed_sequence;
        uint8_t parsed_type;
        int status = decode_block_entry(block, offset, key, sizeof(key),
                                        &key_length, &entry);

        if (status != 0 || entry.next_offset <= offset ||
            slsm_nova_internal_key_decode(key, key_length, &user_key,
                                          &user_key_length, &parsed_sequence,
                                          &parsed_type) != 0 ||
            (count != 0 && slsm_nova_internal_key_compare(
                               previous_key, previous_key_length, key,
                               key_length) >= 0))
            return -EBADMSG;
        if (!found && lookup_user_key != NULL && parsed_sequence <= snapshot &&
            compare_bytes(user_key, user_key_length, lookup_user_key,
                          lookup_user_key_length) == 0) {
            if (value != NULL)
                *value = entry.value;
            if (value_length != NULL)
                *value_length = entry.value_length;
            if (value_type != NULL)
                *value_type = parsed_type;
            if (sequence != NULL)
                *sequence = parsed_sequence;
            found = 1;
        }
        memcpy(previous_key, key, key_length);
        previous_key_length = key_length;
        offset = entry.next_offset;
        ++count;
    }
    if (offset != block->entries_end || count == 0)
        return -EBADMSG;
    if (entry_count != NULL)
        *entry_count = count;
    if (lookup_user_key != NULL && !found)
        return -ENOENT;
    return 0;
}

int slsm_nova_table_validate(const void *buffer, size_t length,
                             struct slsm_nova_table_info *info)
{
    const uint8_t *table = buffer;
    struct nova_table_layout layout;
    struct nova_block_view data;
    size_t entry_count;
    int status;

    if (buffer == NULL)
        return -EINVAL;
    status = parse_table_layout(table, length, &layout);
    if (status == 0)
        status = open_block(table, length, &layout.data, &data);
    if (status == 0)
        status = scan_data_block(&data, NULL, 0, 0, NULL, NULL, NULL, NULL,
                                 &entry_count);
    if (status == 0 && info != NULL)
        *info = (struct slsm_nova_table_info){
            .entry_count = entry_count,
            .data_block_count = 1,
        };
    return status;
}

int slsm_nova_table_lookup(const void *buffer, size_t length,
                           const uint8_t *user_key, size_t user_key_length,
                           uint64_t snapshot, const uint8_t **value,
                           size_t *value_length, uint8_t *value_type,
                           uint64_t *sequence)
{
    const uint8_t *table = buffer;
    struct nova_table_layout layout;
    struct nova_block_view data;
    int status;

    if (buffer == NULL || (user_key == NULL && user_key_length != 0) ||
        snapshot > SLSM_NOVA_MAX_SEQUENCE || value == NULL ||
        value_length == NULL)
        return -EINVAL;
    status = parse_table_layout(table, length, &layout);
    if (status == 0)
        status = open_block(table, length, &layout.data, &data);
    if (status == 0)
        status = scan_data_block(&data, user_key, user_key_length, snapshot,
                                 value, value_length, value_type, sequence, NULL);
    return status;
}
