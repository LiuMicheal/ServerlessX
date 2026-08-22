#include "slsm_compaction.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static void add_record(struct slsm_memtable *memtable, uint64_t key,
                       uint64_t value)
{
    assert(slsm_memtable_put(memtable, key, value) == 0);
}

int main(void)
{
    unsigned char first[SLSM_SST_MAX_SIZE];
    unsigned char second[SLSM_SST_MAX_SIZE];
    unsigned char batch[SLSM_MAX_SST_SIZE];
    unsigned char output[SLSM_SST_MAX_SIZE];
    struct slsm_memtable first_memtable;
    struct slsm_memtable second_memtable;
    struct slsm_compaction_batch_header batch_header;
    struct slsm_sst_metadata first_metadata;
    struct slsm_sst_metadata second_metadata;
    struct slsm_sst_metadata output_metadata;
    struct slsm_sst_header output_header;
    size_t first_length;
    size_t second_length;
    size_t batch_length;
    uint64_t overlap_key;
    uint64_t overlap_value;
    uint64_t value;

    slsm_memtable_init(&first_memtable);
    add_record(&first_memtable, 100, 1000);
    add_record(&first_memtable, 101, 1001);
    add_record(&first_memtable, 102, 1002);
    slsm_memtable_init(&second_memtable);
    add_record(&second_memtable, 101, 2001);
    add_record(&second_memtable, 102, 2002);
    add_record(&second_memtable, 103, 2003);
    assert(slsm_sst_flush(&first_memtable, 11, 7, first, sizeof(first),
                          &first_length, &first_metadata) == 0);
    assert(slsm_sst_flush(&second_memtable, 12, 7, second, sizeof(second),
                          &second_length, &second_metadata) == 0);
    assert(slsm_compaction_pack(first, first_length, 11, second, second_length,
                                12, 77, 13, 7, batch, sizeof(batch),
                                &batch_length) == 0);
    assert(slsm_compaction_validate(batch, batch_length, &batch_header) == 0);
    assert(batch_header.output_sst_id == 13 && batch_header.input_count == 2);
    assert(slsm_compaction_merge(batch, batch_length, output, sizeof(output),
                                 &output_metadata, &output_header, &overlap_key,
                                 &overlap_value) == 0);
    assert(output_metadata.level == 1 && output_metadata.epoch == 7);
    assert(output_header.sst_id == 13 && output_header.record_count == 4);
    assert(overlap_key == 101 && overlap_value == 2001);
    assert(slsm_sst_validate(output, sizeof(struct slsm_sst_header) +
                             (size_t)output_header.payload_bytes, 13, 7,
                             NULL) == 0);
    assert(slsm_sst_lookup(output, sizeof(struct slsm_sst_header) +
                           (size_t)output_header.payload_bytes, 101, &value) == 0 &&
           value == 2001);
    assert(slsm_sst_lookup(output, sizeof(struct slsm_sst_header) +
                           (size_t)output_header.payload_bytes, 100, &value) == 0 &&
           value == 1000);

    printf("{\"event\":\"compaction_test\",\"status\":\"pass\","
           "\"inputs\":2,\"output_level\":%u,\"output_records\":%u,"
           "\"overlap_key\":%" PRIu64 ",\"overlap_value\":%" PRIu64 "}\n",
           output_metadata.level, output_header.record_count, overlap_key,
           overlap_value);
    return 0;
}
