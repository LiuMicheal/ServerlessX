#include "slsm_manifest.h"

#include <errno.h>
#include <string.h>

static int valid_metadata(const struct slsm_sst_metadata *metadata)
{
    return metadata != NULL && metadata->version == SLSM_METADATA_VERSION &&
           metadata->level <= SLSM_MAX_SST_LEVEL && metadata->epoch != 0 &&
           metadata->min_key <= metadata->max_key;
}

static int valid_descriptor(const struct slsm_sst_descriptor *descriptor)
{
    return descriptor != NULL && descriptor->version == SLSM_ABI_VERSION &&
           descriptor->sst_id != 0 && descriptor->chunk_count > 0 &&
           descriptor->chunk_count <= SLSM_MAX_CHUNKS &&
           descriptor->total_length > 0 &&
           descriptor->total_length <= SLSM_MAX_SST_SIZE &&
           descriptor->chunk_size == SLSM_CHUNK_SIZE;
}

void slsm_manifest_init(struct slsm_manifest *manifest)
{
    memset(manifest, 0, sizeof(*manifest));
}

int slsm_manifest_publish(struct slsm_manifest *manifest,
                          const struct slsm_sst_metadata *metadata,
                          const struct slsm_sst_descriptor *descriptor)
{
    struct slsm_manifest_entry *entry;
    uint32_t index;

    if (manifest == NULL || !valid_descriptor(descriptor) ||
        !valid_metadata(metadata))
        return -EINVAL;
    for (index = 0; index < manifest->count; ++index) {
        if (manifest->entries[index].descriptor.sst_id == descriptor->sst_id)
            return -EEXIST;
    }
    if (manifest->count == SLSM_MANIFEST_MAX_ENTRIES)
        return -ENOSPC;

    entry = &manifest->entries[manifest->count++];
    entry->state = SLSM_MANIFEST_PENDING;
    entry->metadata = *metadata;
    entry->descriptor = *descriptor;
    ++manifest->version;
    return 0;
}

int slsm_manifest_commit(struct slsm_manifest *manifest, uint64_t sst_id,
                         uint64_t epoch)
{
    uint32_t index;

    if (manifest == NULL || epoch == 0)
        return -EINVAL;
    for (index = 0; index < manifest->count; ++index) {
        struct slsm_manifest_entry *entry = &manifest->entries[index];

        if (entry->descriptor.sst_id != sst_id)
            continue;
        if (entry->state != SLSM_MANIFEST_PENDING || entry->metadata.epoch != epoch)
            return -EINVAL;
        entry->state = SLSM_MANIFEST_COMMITTED;
        ++manifest->version;
        return 0;
    }
    return -ENOENT;
}

int slsm_manifest_revoke(struct slsm_manifest *manifest, uint64_t sst_id)
{
    uint32_t index;

    if (manifest == NULL)
        return -EINVAL;
    for (index = 0; index < manifest->count; ++index) {
        if (manifest->entries[index].descriptor.sst_id != sst_id)
            continue;
        if (manifest->entries[index].state != SLSM_MANIFEST_COMMITTED)
            return -EINVAL;
        if (index + 1 < manifest->count) {
            memmove(&manifest->entries[index], &manifest->entries[index + 1],
                    (manifest->count - index - 1) * sizeof(manifest->entries[0]));
        }
        --manifest->count;
        memset(&manifest->entries[manifest->count], 0,
               sizeof(manifest->entries[manifest->count]));
        ++manifest->version;
        return 0;
    }
    return -ENOENT;
}

int slsm_manifest_abort(struct slsm_manifest *manifest, uint64_t sst_id)
{
    uint32_t index;

    if (manifest == NULL || sst_id == 0)
        return -EINVAL;
    for (index = 0; index < manifest->count; ++index) {
        if (manifest->entries[index].descriptor.sst_id != sst_id)
            continue;
        if (index + 1 < manifest->count) {
            memmove(&manifest->entries[index], &manifest->entries[index + 1],
                    (manifest->count - index - 1) * sizeof(manifest->entries[0]));
        }
        --manifest->count;
        memset(&manifest->entries[manifest->count], 0,
               sizeof(manifest->entries[manifest->count]));
        ++manifest->version;
        return 0;
    }
    return -ENOENT;
}

const struct slsm_manifest_entry *slsm_manifest_lookup(
    const struct slsm_manifest *manifest, uint64_t key, uint64_t epoch)
{
    const struct slsm_manifest_entry *best = NULL;
    uint32_t index;

    if (manifest == NULL || epoch == 0)
        return NULL;
    for (index = 0; index < manifest->count; ++index) {
        const struct slsm_manifest_entry *entry = &manifest->entries[index];

        if (entry->state != SLSM_MANIFEST_COMMITTED ||
            entry->metadata.epoch > epoch || key < entry->metadata.min_key ||
            key > entry->metadata.max_key)
            continue;
        if (best == NULL || entry->metadata.epoch > best->metadata.epoch ||
            (entry->metadata.epoch == best->metadata.epoch &&
             (entry->metadata.level < best->metadata.level ||
              (entry->metadata.level == best->metadata.level &&
               entry->descriptor.sst_id > best->descriptor.sst_id))))
            best = entry;
    }
    return best;
}
