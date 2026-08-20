#ifndef SLSM_UAPI_H
#define SLSM_UAPI_H

#include <stdint.h>

#define SLSM_ABI_VERSION 1U
#define SLSM_MAX_CHUNKS 8U
#define SLSM_CHUNK_SIZE (1024U * 1024U)
#define SLSM_MAX_SST_SIZE (SLSM_MAX_CHUNKS * SLSM_CHUNK_SIZE)

#define SLSM_IOCTL_CONNECT_PEER 0x534c0001U
#define SLSM_IOCTL_REGISTER_REGION 0x534c0002U
#define SLSM_IOCTL_FETCH_REGION 0x534c0003U
#define SLSM_IOCTL_UNREGISTER_REGION 0x534c0004U
#define SLSM_IOCTL_DISCONNECT 0x534c0005U

#define SLSM_CONTROL_MAGIC 0x534c534dU
#define SLSM_CONTROL_PORT 18515U
#define SLSM_LIFECYCLE_VERSION 1U

#define SLSM_PHASE_PUBLISH 1U
#define SLSM_PHASE_FETCH 2U
#define SLSM_PHASE_COMMIT 3U
#define SLSM_PHASE_REVOKE 4U

struct slsm_remote_chunk {
    uint64_t remote_addr;
    uint32_t length;
    uint32_t rkey;
};

struct slsm_sst_descriptor {
    uint32_t version;
    uint32_t chunk_count;
    uint64_t sst_id;
    uint64_t total_length;
    uint32_t chunk_size;
    uint32_t reserved;
    uint64_t checksum;
    uint8_t owner_gid[16];
    uint64_t service_id;
    struct slsm_remote_chunk chunks[SLSM_MAX_CHUNKS];
};

struct slsm_connect_req {
    uint32_t version;
    uint32_t reserved;
    uint8_t peer_gid[16];
    uint64_t service_id;
};

struct slsm_register_req {
    uint64_t user_addr;
    uint64_t length;
    uint64_t sst_id;
    struct slsm_sst_descriptor descriptor;
};

struct slsm_fetch_req {
    uint64_t user_addr;
    uint64_t capacity;
    struct slsm_sst_descriptor descriptor;
    uint64_t fetched_length;
    uint64_t checksum;
    uint64_t elapsed_us;
};

struct slsm_control_message {
    uint32_t magic;
    uint32_t version;
    struct slsm_sst_descriptor descriptor;
};

struct slsm_control_ack {
    uint32_t magic;
    int32_t status;
    uint64_t fetched_length;
    uint64_t checksum;
    uint64_t elapsed_us;
};

struct slsm_lifecycle_message {
    uint32_t magic;
    uint32_t version;
    uint32_t phase;
    uint32_t reserved;
    uint64_t generation;
    struct slsm_sst_descriptor descriptor;
};

struct slsm_lifecycle_ack {
    uint32_t magic;
    uint32_t version;
    uint32_t phase;
    int32_t status;
    uint64_t generation;
    uint64_t fetched_length;
    uint64_t checksum;
    uint64_t elapsed_us;
};

#endif
