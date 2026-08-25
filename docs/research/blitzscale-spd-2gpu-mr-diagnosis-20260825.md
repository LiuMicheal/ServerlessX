# BlitzScale SPD Two-Guest Smoke and MR Diagnosis

Date: 2026-08-25

## Observed boundary

- The two BlitzScale workers load the Llama-3 FP16 model and start gRPC.
- The router starts with one Prefill and one Decode replica; its health endpoints return HTTP 200.
- The normal worker path aborts while registering the first CUDA buffer with:

```text
Failed to register memory region
```

- A temporary `BLITZSCALE_SKIP_MR=1` mode starts both workers, but has no lkey/rkey and is not an RDMA or inference result.

## Root cause

The stock BlitzScale pickle backend registers every GPU segment through
`ibv_reg_mr(pd, cuda_pointer, length, access_flags)`. The segments are allocated
with the legacy `cudaMalloc` allocator. This is the legacy GPUDirect RDMA
peer-memory path and requires a working NVIDIA peer-memory client in the Guest.

The current Guest does not have `nvidia_peermem` loaded; a prior load attempt
returned `EINVAL`. Consequently the verbs provider cannot translate the CUDA
virtual address for `ibv_reg_mr`, and registration fails before any cross-node
transfer is attempted.

This does not contradict the SPD GDR result. SPD uses a different path:

```text
CUDA VMM allocation -> DMA-BUF FD -> ibv_reg_dmabuf_mr -> RC transfer
```

That path is backed by the native SPD GDR implementation and is not exercised
by BlitzScale's stock `ibv_reg_mr` call. Simply copying the SPD throughput result
therefore cannot validate BlitzScale's current backend.

## Correct next implementation step

Prefer adapting BlitzScale's registered buffers (weights, KV/runtime, and
activation segments) to CUDA VMM allocations, exporting DMA-BUF handles, and
registering them with `ibv_reg_dmabuf_mr` using role-specific access flags.
Retain a separate legacy `ibv_reg_mr` fallback only when the Guest's
`nvidia_peermem` compatibility is explicitly verified. Also preserve `errno`
in the error path so future failures identify the kernel/provider rejection.

No real RDMA/GDR throughput or generated-token result is claimed by this note.
