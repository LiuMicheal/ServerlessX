#ifndef SLSM_MANIFEST_H
#define SLSM_MANIFEST_H

#include <stddef.h>
#include <stdint.h>

#include "include/slsm_uapi.h"

#define SLSM_MANIFEST_MAX_ENTRIES 8U

#define SLSM_MANIFEST_PENDING 1U
#define SLSM_MANIFEST_COMMITTED 2U

struct slsm_manifest_entry {
    uint32_t state;
    uint32_t reserved;
    struct slsm_sst_metadata metadata;
    struct slsm_sst_descriptor descriptor;
};

struct slsm_manifest {
    uint64_t version;
    uint32_t count;
    uint32_t reserved;
    struct slsm_manifest_entry entries[SLSM_MANIFEST_MAX_ENTRIES];
};

void slsm_manifest_init(struct slsm_manifest *manifest);
int slsm_manifest_publish(struct slsm_manifest *manifest,
                          const struct slsm_sst_metadata *metadata,
                          const struct slsm_sst_descriptor *descriptor);
int slsm_manifest_commit(struct slsm_manifest *manifest, uint64_t sst_id,
                         uint64_t epoch);
int slsm_manifest_revoke(struct slsm_manifest *manifest, uint64_t sst_id);
int slsm_manifest_abort(struct slsm_manifest *manifest, uint64_t sst_id);
const struct slsm_manifest_entry *slsm_manifest_lookup(
    const struct slsm_manifest *manifest, uint64_t key, uint64_t epoch);

#endif
