#include "slsm_nova_sst.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static int make_entry(struct slsm_nova_entry *entry, const char *key,
                      const char *value, uint64_t sequence)
{
    *entry = (struct slsm_nova_entry){
        .user_key = (const uint8_t *)key,
        .user_key_length = strlen(key),
        .value = (const uint8_t *)value,
        .value_length = strlen(value),
        .sequence = sequence,
        .type = SLSM_NOVA_TYPE_VALUE,
    };
    return 0;
}

int main(void)
{
    struct slsm_nova_entry entries[4];
    struct slsm_nova_entry max_entries[32];
    struct slsm_nova_table_info info;
    uint8_t internal_key[32];
    uint8_t max_keys[32][8];
    uint8_t max_values[32][8];
    uint8_t table[2048];
    const uint8_t *value;
    size_t internal_length;
    size_t value_length;
    size_t table_length;
    uint64_t sequence;
    uint8_t type;

    assert(slsm_nova_internal_key_encode((const uint8_t *)"alpha", 5, 17,
                                         SLSM_NOVA_TYPE_VALUE, internal_key,
                                         sizeof(internal_key), &internal_length) == 0);
    assert(internal_length == 13);
    assert(slsm_nova_internal_key_decode(
               internal_key, internal_length, &value, &value_length, &sequence,
               &type) == 0);
    assert(value == internal_key && value_length == 5 && sequence == 17 &&
           type == SLSM_NOVA_TYPE_VALUE);
    assert(slsm_nova_internal_key_compare(internal_key, internal_length,
                                          internal_key, internal_length) == 0);

    make_entry(&entries[0], "alpha", "v3", 3);
    make_entry(&entries[1], "alpha", "v2", 2);
    make_entry(&entries[2], "alphabet", "v1", 1);
    make_entry(&entries[3], "beta", "v4", 4);
    assert(slsm_nova_table_build(entries, 4, table, sizeof(table), &table_length,
                                 &info) == 0);
    assert(info.entry_count == 4 && info.data_block_count == 1);
    assert(slsm_nova_table_validate(table, table_length, &info) == 0);
    assert(info.entry_count == 4 && info.data_block_count == 1);
    assert(slsm_nova_table_lookup(table, table_length, (const uint8_t *)"alpha",
                                  5, SLSM_NOVA_MAX_SEQUENCE, &value,
                                  &value_length, &type, &sequence) == 0);
    assert(value_length == 2 && memcmp(value, "v3", 2) == 0 &&
           type == SLSM_NOVA_TYPE_VALUE && sequence == 3);
    assert(slsm_nova_table_lookup(table, table_length, (const uint8_t *)"alpha",
                                  5, 2, &value, &value_length, &type,
                                  &sequence) == 0);
    assert(value_length == 2 && memcmp(value, "v2", 2) == 0 && sequence == 2);
    assert(slsm_nova_table_lookup(table, table_length, (const uint8_t *)"missing",
                                  7, SLSM_NOVA_MAX_SEQUENCE, &value,
                                  &value_length, &type, &sequence) == -ENOENT);

    for (size_t index = 0; index < 32; ++index) {
        for (size_t byte = 0; byte < sizeof(max_keys[index]); ++byte) {
            max_keys[index][byte] = (uint8_t)(index >> (56 - byte * 8));
            max_values[index][byte] = (uint8_t)(index + byte);
        }
        max_entries[index] = (struct slsm_nova_entry){
            .user_key = max_keys[index],
            .user_key_length = sizeof(max_keys[index]),
            .value = max_values[index],
            .value_length = sizeof(max_values[index]),
            .sequence = SLSM_NOVA_MAX_SEQUENCE,
            .type = SLSM_NOVA_TYPE_VALUE,
        };
    }
    assert(slsm_nova_table_build(max_entries, 32, table, sizeof(table),
                                 &table_length, &info) == 0);
    assert(table_length <= sizeof(table) && info.entry_count == 32);
    assert(slsm_nova_table_validate(table, table_length, &info) == 0);
    assert(info.entry_count == 32);

    table[0] ^= UINT8_C(1);
    assert(slsm_nova_table_validate(table, table_length, NULL) == -EBADMSG);

    printf("{\"event\":\"nova_sst_test\",\"status\":\"pass\","
           "\"entries\":%zu,\"bytes\":%zu,\"lookup_sequence\":%" PRIu64
           "}\n",
           info.entry_count, table_length, sequence);
    return 0;
}
