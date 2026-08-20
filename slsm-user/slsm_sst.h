#ifndef SLSM_SST_H
#define SLSM_SST_H

#include <stddef.h>
#include <stdint.h>

#include "include/slsm_uapi.h"

#define SLSM_SST_MAGIC UINT32_C(0x53535431)
#define SLSM_SST_VERSION 2U
#define SLSM_SST_MAX_RECORDS 32U

/*
 * The payload is a small clean-room LevelDB/Nova table: one data block, an
 * empty metaindex block, one index block, and the standard 48-byte footer.
 * The bound keeps the test SST comfortably below the existing 8 MiB RDMA
 * descriptor limit while leaving room for worst-case varint encodings.
 */
#define SLSM_NOVA_TABLE_MAX_SIZE 2048U

struct slsm_sst_record {
    uint64_t key;
    uint64_t value;
};

struct slsm_sst_header {
    uint32_t magic;
    uint16_t version;
    uint16_t record_count;
    uint64_t sst_id;
    uint64_t epoch;
    uint64_t min_key;
    uint64_t max_key;
    uint64_t payload_bytes;
    uint64_t reserved;
};

#define SLSM_SST_MAX_SIZE \
    (sizeof(struct slsm_sst_header) + SLSM_NOVA_TABLE_MAX_SIZE)

struct slsm_memtable {
    size_t count;
    struct slsm_sst_record records[SLSM_SST_MAX_RECORDS];
};

void slsm_memtable_init(struct slsm_memtable *memtable);
int slsm_memtable_put(struct slsm_memtable *memtable, uint64_t key, uint64_t value);
size_t slsm_sst_encoded_size(size_t record_count);
int slsm_sst_flush(const struct slsm_memtable *memtable, uint64_t sst_id,
                  uint64_t epoch, void *buffer, size_t capacity,
                  size_t *encoded_length, struct slsm_sst_metadata *metadata);
int slsm_sst_validate(const void *buffer, size_t length, uint64_t expected_sst_id,
                     uint64_t expected_epoch, struct slsm_sst_header *header);
int slsm_sst_lookup(const void *buffer, size_t length, uint64_t key,
                    uint64_t *value);

#endif
