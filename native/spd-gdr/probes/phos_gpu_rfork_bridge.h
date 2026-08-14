#ifndef SERVERLESSX_PHOS_GPU_RFORK_BRIDGE_H
#define SERVERLESSX_PHOS_GPU_RFORK_BRIDGE_H

#include "serverlessx/rfork_runtime.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct PhosSessionIdentityV1 {
    uint32_t abi_version;
    uint32_t actual_pid;
    int32_t remoting_id;
    uint32_t session_kind;
    uint64_t phos_uuid;
};

struct PhosCloneMemoryChildIdentityV2 {
    uint32_t abi_version;
    uint32_t actual_pid;
    int32_t remoting_id;
    uint32_t replay_required;
    uint64_t source_phos_uuid;
    uint64_t target_phos_uuid;
    uint64_t clone_ticket_hash;
    uint64_t image_bytes;
    uint32_t record_count;
    uint32_t reserved;
    uint8_t sha256[32];
};

typedef int (*SpdPhosQuerySessionFn)(
    struct PhosSessionIdentityV1 *, size_t);
typedef int (*SpdPhosCloneAttachMemoryFn)(
    const char *, const void *, uint64_t, const uint8_t *, const char *,
    const char *, struct PhosCloneMemoryChildIdentityV2 *, size_t);

enum spd_gpu_rfork_bridge_status {
    SPD_GPU_RFORK_BRIDGE_OK = 0,
    SPD_GPU_RFORK_BRIDGE_INVALID_ARGUMENT = 1,
    SPD_GPU_RFORK_BRIDGE_RFORK_ERROR = 2,
    SPD_GPU_RFORK_BRIDGE_PHOS_ERROR = 3,
    SPD_GPU_RFORK_BRIDGE_OWNERSHIP_ERROR = 4
};

struct SpdGpuRforkBridgeResult {
    uint32_t abi_version;
    int32_t status;
    int32_t native_status;
    uint32_t role;
    uint32_t handler_id;
    uint32_t reserved;
    uint64_t clone_attach_ns;
    struct PhosCloneMemoryChildIdentityV2 child;
    struct PhosSessionIdentityV1 session;
};

int spd_gpu_rfork_prepare_attach(
    uint64_t handler_id,
    uint32_t source_machine_id,
    uint32_t target_machine_id,
    const uint8_t run_cookie[SPD_RFORK_RUN_COOKIE_SIZE],
    const char *clone_ticket,
    const void *image_address,
    uint64_t image_mapping_bytes,
    uint64_t image_bytes,
    uint32_t image_record_count,
    const uint8_t image_sha256[32],
    const char *target_network_config,
    const char *target_pos_config,
    uint64_t source_phos_uuid,
    void *cuda_allocation,
    uint64_t cuda_allocation_bytes,
    uint64_t source_digest,
    struct SpdGpuRforkBridgeResult *result);

int spd_gpu_rfork_parent_release_bridge(void);

#ifdef __cplusplus
}
#endif

#endif
