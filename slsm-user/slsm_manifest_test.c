#include "slsm_manifest.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>

static struct slsm_sst_descriptor descriptor(uint64_t sst_id)
{
    return (struct slsm_sst_descriptor){
        .version = SLSM_ABI_VERSION,
        .chunk_count = 1,
        .sst_id = sst_id,
        .total_length = 1,
        .chunk_size = SLSM_CHUNK_SIZE,
    };
}

static struct slsm_sst_metadata metadata(uint64_t epoch, uint64_t min_key,
                                         uint64_t max_key)
{
    return (struct slsm_sst_metadata){
        .version = SLSM_METADATA_VERSION,
        .level = 0,
        .epoch = epoch,
        .min_key = min_key,
        .max_key = max_key,
    };
}

int main(void)
{
    struct slsm_manifest manifest;
    struct slsm_sst_descriptor first = descriptor(101);
    struct slsm_sst_descriptor second = descriptor(102);
    struct slsm_sst_metadata first_meta = metadata(1, 0, 99);
    struct slsm_sst_metadata second_meta = metadata(1, 50, 149);
    const struct slsm_manifest_entry *entry;

    slsm_manifest_init(&manifest);
    first_meta.level = 1;
    assert(slsm_manifest_publish(&manifest, &first_meta, &first) == 0);
    assert(slsm_manifest_lookup(&manifest, 75, 1) == NULL);
    assert(slsm_manifest_commit(&manifest, first.sst_id, 2) == -EINVAL);
    assert(slsm_manifest_revoke(&manifest, first.sst_id) == -EINVAL);
    assert(slsm_manifest_publish(&manifest, &first_meta, &first) == -EEXIST);
    assert(slsm_manifest_publish(&manifest, &second_meta, &second) == 0);
    assert(slsm_manifest_commit(&manifest, first.sst_id, first_meta.epoch) == 0);
    assert(slsm_manifest_commit(&manifest, second.sst_id, second_meta.epoch) == 0);

    entry = slsm_manifest_lookup(&manifest, 75, 1);
    assert(entry != NULL && entry->descriptor.sst_id == second.sst_id);

    assert(slsm_manifest_revoke(&manifest, second.sst_id) == 0);
    entry = slsm_manifest_lookup(&manifest, 75, 1);
    assert(entry != NULL && entry->descriptor.sst_id == first.sst_id);
    assert(slsm_manifest_revoke(&manifest, first.sst_id) == 0);
    assert(slsm_manifest_lookup(&manifest, 75, 2) == NULL);

    printf("{\"event\":\"manifest_test\",\"status\":\"pass\","
           "\"entries\":2,\"lookup_key\":75,\"final_count\":%u,"
           "\"version\":%" PRIu64 "}\n",
           manifest.count, manifest.version);
    return 0;
}
