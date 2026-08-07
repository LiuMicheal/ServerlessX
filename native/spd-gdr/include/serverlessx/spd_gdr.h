// Copyright 2026 ServerlessPD Artifact Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t spd_gdr_abi_version(void);
const char *spd_gdr_last_error(void);

// The caller owns and must strongly retain the CUDA allocation until a
// successful close. Handles are single-owner and must not be reused after
// close. The library never allocates, copies, or frees the buffer. The caller
// must synchronize producer CUDA work before transfer. Open enables and
// verifies CU_POINTER_ATTRIBUTE_SYNC_MEMOPS on the allocation. A target caller
// must wait for transfer completion and synchronize before consuming it.
void *spd_gdr_open(const char *local_server_name, uint64_t device_pointer,
                   uint64_t bytes, int gpu_id);

enum spd_gdr_dmabuf_role_v1 {
    SPD_GDR_DMABUF_SOURCE_V1 = 1,
    SPD_GDR_DMABUF_TARGET_V1 = 2,
};

// Register a caller-owned CUDA VMM allocation directly through its DMA-BUF.
// The endpoint is an IPv4 address and TCP port (for example,
// "192.0.2.12:23140"); TCP carries only RC QP/MR metadata. A source serves
// one target in a bounded background thread (300 s by default, at most 600 s
// via SPD_GDR_DIRECT_CONTROL_TIMEOUT_MS). A target initiates the native RDMA
// READ when spd_gdr_read() is called. This path does not use Mooncake.
void *spd_gdr_open_dmabuf_v1(const char *local_endpoint,
                             uint64_t device_pointer, uint64_t bytes,
                             int gpu_id, int role);

int spd_gdr_transfer(void *context, const char *target_server_name,
                     uint64_t timeout_ms, uint64_t *metadata_exchange_ns,
                     uint64_t *submit_to_completion_ns,
                     uint64_t *transferred_bytes);

// Read the target segment into the caller-owned local CUDA allocation.  This
// is an additive ABI extension; spd_gdr_transfer keeps its original WRITE
// semantics for existing callers.
int spd_gdr_read(void *context, const char *source_server_name,
                 uint64_t timeout_ms, uint64_t *metadata_exchange_ns,
                 uint64_t *submit_to_completion_ns,
                 uint64_t *transferred_bytes);

/*
 * Read one bounded subrange between equally-sized registered staging slabs.
 * The local and remote offsets are relative to the buffers passed to
 * spd_gdr_open().  This keeps one MR/TransferEngine alive while PhOS restores
 * many CUDA regions from a single aggregate image.
 */
int spd_gdr_read_at_v1(void *context, const char *source_server_name,
                       uint64_t local_offset, uint64_t remote_offset,
                       uint64_t bytes, uint64_t timeout_ms,
                       uint64_t *metadata_exchange_ns,
                       uint64_t *submit_to_completion_ns,
                       uint64_t *transferred_bytes);

// Return values for transfer and close: 0 success, -1 ordinary failure, -2 an
// unretired transfer or poisoned context. On -2 the caller must keep its CUDA
// allocation alive and immediately terminate the owning process; it must not
// retry, close, or allow the CUDA allocation to undergo normal teardown.
int spd_gdr_close(void *context);

#ifdef __cplusplus
}
#endif
