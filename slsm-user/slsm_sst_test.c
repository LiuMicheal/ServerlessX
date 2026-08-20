#include "slsm_sst.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>

int main(void)
{
    unsigned char buffer[SLSM_SST_MAX_SIZE];
    struct slsm_memtable memtable;
    struct slsm_sst_metadata metadata;
    struct slsm_sst_header header;
    size_t length;
    uint64_t value;

    slsm_memtable_init(&memtable);
    assert(slsm_memtable_put(&memtable, 30, 300) == 0);
    assert(slsm_memtable_put(&memtable, 10, 100) == 0);
    assert(slsm_memtable_put(&memtable, 20, 200) == 0);
    assert(slsm_memtable_put(&memtable, 20, 222) == 0);
    assert(memtable.count == 3 && memtable.records[0].key == 10);
    assert(slsm_sst_flush(&memtable, 700, 9, buffer, sizeof(buffer), &length,
                          &metadata) == 0);
    assert(metadata.min_key == 10 && metadata.max_key == 30);
    assert(slsm_sst_validate(buffer, length, 700, 9, &header) == 0);
    assert(header.record_count == 3);
    assert(slsm_sst_lookup(buffer, length, 20, &value) == 0 && value == 222);
    assert(slsm_sst_lookup(buffer, length, 25, &value) == -ENOENT);
    assert(slsm_sst_validate(buffer, length - 1, 700, 9, NULL) == -EBADMSG);

    printf("{\"event\":\"sst_test\",\"status\":\"pass\","
           "\"records\":%u,\"bytes\":%zu,\"lookup_key\":20,"
           "\"lookup_value\":%" PRIu64 "}\n",
           header.record_count, length, value);
    return 0;
}
