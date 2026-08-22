#ifndef SLSM_COMPACTION_H
#define SLSM_COMPACTION_H

#include <stddef.h>
#include <stdint.h>

#include "slsm_sst.h"

#define SLSM_COMPACTION_MAGIC UINT32_C(0x53434d50)
#define SLSM_COMPACTION_VERSION 1U
#define SLSM_COMPACTION_INPUTS 2U

struct slsm_compaction_input {
    uint64_t offset;
    uint64_t length;
    uint64_t sst_id;
};

struct slsm_compaction_batch_header {
    uint32_t magic;
    uint16_t version;
    uint16_t input_count;
    uint32_t header_size;
    uint32_t reserved;
    uint64_t job_id;
    uint64_t output_sst_id;
    uint64_t epoch;
    struct slsm_compaction_input inputs[SLSM_COMPACTION_INPUTS];
};

int slsm_compaction_pack(const void *first, size_t first_length,
                         uint64_t first_sst_id, const void *second,
                         size_t second_length, uint64_t second_sst_id,
                         uint64_t job_id, uint64_t output_sst_id, uint64_t epoch,
                         void *buffer, size_t capacity, size_t *encoded_length);
int slsm_compaction_validate(const void *buffer, size_t length,
                             struct slsm_compaction_batch_header *header);
int slsm_compaction_merge(const void *buffer, size_t length, void *output,
                          size_t capacity, struct slsm_sst_metadata *metadata,
                          struct slsm_sst_header *output_header,
                          uint64_t *overlap_key, uint64_t *overlap_value);

#endif
