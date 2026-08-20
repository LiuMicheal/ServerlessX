#ifndef SLSM_NOVA_SST_H
#define SLSM_NOVA_SST_H

/*
 * A small, dependency-free subset of the Nova/LevelDB table format.
 *
 * This interface deliberately contains no SLSM ioctl or RDMA types.  The
 * caller owns the outer SLSM header; this module only builds and reads the
 * table payload placed after that header.  The payload uses uncompressed
 * prefix-compressed data/index blocks, an empty metaindex block, CRC32C block
 * trailers, and the standard 48-byte LevelDB footer.  It intentionally omits
 * filters and multi-block partitioning, which are not needed by the small
 * Stage-1 proof of concept.
 */

#include <stddef.h>
#include <stdint.h>

#define SLSM_NOVA_TABLE_MAGIC UINT64_C(0xdb4775248b80fb57)
#define SLSM_NOVA_FOOTER_SIZE 48U
#define SLSM_NOVA_BLOCK_TRAILER_SIZE 5U
#define SLSM_NOVA_RESTART_INTERVAL 16U
#define SLSM_NOVA_MAX_SEQUENCE ((UINT64_C(1) << 56) - UINT64_C(1))

enum slsm_nova_value_type {
    SLSM_NOVA_TYPE_DELETION = 0,
    SLSM_NOVA_TYPE_VALUE = 1,
};

struct slsm_nova_entry {
    const uint8_t *user_key;
    size_t user_key_length;
    const uint8_t *value;
    size_t value_length;
    uint64_t sequence;
    uint8_t type;
};

struct slsm_nova_table_info {
    size_t entry_count;
    size_t data_block_count;
};

/* Encode/decode the LevelDB internal-key suffix and user-key prefix. */
int slsm_nova_internal_key_encode(const uint8_t *user_key, size_t user_key_length,
                                  uint64_t sequence, uint8_t type,
                                  uint8_t *output, size_t capacity,
                                  size_t *encoded_length);
int slsm_nova_internal_key_decode(const uint8_t *internal_key,
                                  size_t internal_key_length,
                                  const uint8_t **user_key,
                                  size_t *user_key_length, uint64_t *sequence,
                                  uint8_t *type);

/* Compare according to the Nova/LevelDB internal-key ordering. */
int slsm_nova_internal_key_compare(const uint8_t *left, size_t left_length,
                                   const uint8_t *right, size_t right_length);

/* Build, validate, and look up a dependency-free Nova-compatible table. */
int slsm_nova_table_build(const struct slsm_nova_entry *entries, size_t entry_count,
                          void *buffer, size_t capacity, size_t *encoded_length,
                          struct slsm_nova_table_info *info);
int slsm_nova_table_validate(const void *buffer, size_t length,
                             struct slsm_nova_table_info *info);
int slsm_nova_table_lookup(const void *buffer, size_t length,
                           const uint8_t *user_key, size_t user_key_length,
                           uint64_t snapshot, const uint8_t **value,
                           size_t *value_length, uint8_t *value_type,
                           uint64_t *sequence);

#endif
