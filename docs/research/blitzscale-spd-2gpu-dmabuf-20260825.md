# BlitzScale SPD Two-Guest DMA-BUF Smoke

Date: 2026-08-25

## Result

The existing two SPD Guests (gpu01 `10.10.12.116`, gpu02 `10.10.12.117`)
completed a real two-rank BlitzScale smoke run:

- MPI used one global rank per Guest with `--map-by ppr:1:node`; the control
  plane was pinned to `eth1`.
- Both ranks selected the Guest-local `mlx5_0`; `world_size=2`.
- The weight, KV, runtime, and activation CUDA buffers all registered through
  `cuMemGetHandleForAddressRange(...DMA_BUF_FD...)` and
  `ibv_reg_dmabuf_mr()`.
- `BLITZSCALE_SKIP_MR` was unset, and the final
  `run_server_disaggregative` ELF does not contain that bypass string.
- Two cross-Guest migrations completed. The router recorded
  `Migrate Batch Done[0]` and `Migrate Batch Done[1]`; rank 0 sent and rank 1
  received 32 KV iterations for each request.
- Both requests returned generated text:
  - `Hello`, `max_new_tokens=4`: `<|begin_of_text|>Hello\uFFFDAAA`
  - `The quick brown fox`, `max_new_tokens=8`:
    `<|begin_of_text|>The quick brown fox\uFFFDA\uFFFD\uFFFD!\uFFFD\uFFFDa`

This is not the earlier `SKIP_MR` startup-only result. DMA-BUF MR
registration, the two-Guest QP topology, cross-Guest KV migration, and token
output all passed in one smoke path.

## Implementation boundary

The BlitzScale GPU MR backend now follows:

```text
CUDA allocation -> CUDA DMA-BUF FD -> ibv_reg_dmabuf_mr()
```

The four buffers were still allocated with `cudaMalloc`. They happened to meet
the page-alignment requirement in this run, so this record must not be read as
a complete `cuMemCreate/cuMemMap` CUDA VMM allocator implementation. A VMM
allocator, complete teardown ordering, large-weight MR chunking, and
performance/stability benchmarks remain follow-up work.

The BlitzScale log reports `bandwidth=infGiBps` because the current millisecond
timer rounded the 32 small transfers to zero elapsed time. This record claims
functional smoke success only; it does not claim a GDR bandwidth result.

## Reproducible evidence

- Evidence summary: `/share/home/liumx/tmp/blitzscale-evidence-20260825.txt`
- Router log: `/home/liumx/projects/blitz-scale/config/router-2gpu-dmabuf-final-20260825.log`
- Final containers: `blitzscale-mpi-gpu01`, `blitzscale-mpi-gpu02`
- Key ELF SHA-256 values (identical on both Guests):

```text
run_server_disaggregative  03f784cf1a4d4a336c3f468ff9df9ebed27b58d92f167ded612579ca93f975ee
libservice.so              aaae0859593ee9d658ca5bb5bb54b45a734bb7a618791fc23d14424bbdb23f4e
librdma_util.so            0dce6b2e8a3bd593d6ecb3d029fbc9e011ca1ea402a0126ca2eaf89e7fe5a6b2
```

Temporary SSH credentials were removed from the containers and are not part
of this record. The earlier `ibv_reg_mr` failure diagnosis remains in
`blitzscale-spd-2gpu-mr-diagnosis-20260825.md` as historical context.
