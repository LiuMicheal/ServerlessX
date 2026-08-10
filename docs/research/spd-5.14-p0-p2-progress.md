# SPD 5.14 P0-P2 progress

Date: 2026-08-10

This note records the public-safe boundary of the current ServerlessPD port.
It does not make the Mitosis or PhOS lab profiles executable.

## Scope

The immediate work is deliberately limited to:

1. freezing the existing GPU/RDMA/PhOS baseline;
2. compiling the external Mitosis backend for the exact SPD guest kernel;
3. running a single-guest CPU fork gate after the module build is accepted.

Cross-guest remote fork, TinyLlama, scheduler work, DCT, and a KRCore rewrite
remain outside this phase.

## P0 evidence boundary

A short canary on the current two SPD guests reproduced ordinary CUDA
DMA-BUF RDMA Write at 98.04 Gb/s in one direction and 98.03 Gb/s in the
other. Both endpoints used `ibv_reg_dmabuf_mr()`.

One archived reverse-direction attempt ended with transport retry exhaustion.
The RDMA links remained active, no test process remained, and the immediate
retry on a new control port completed at 98.03 Gb/s. Both the failed and
successful logs are retained in the private evidence store.

Previously archived evidence was also rechecked:

- bidirectional 2 MiB CUDA VMM DMA-BUF transfer with matching checksums;
- a 2 MiB PhOS-owned direct VMM read with `staging_bytes=0`;
- target clone attach and CUDA digest verification;
- local PhOS checkpoint/restore evidence from both guests.

The cross-guest PhOS source reports a core pass followed by an
abort-after-success teardown. It is not evidence that every teardown path is
already clean.

Raw logs, VM definitions, internal inventory, checkpoint images, binaries,
models, and kernel modules are intentionally excluded from this repository.
They are referenced through private SHA-256 manifests.

## P1 starting point

The port does not restart from Mitosis upstream `main`. The preserved research
integration already contains the pure-RC SPD commits and three compile
attempts:

- the first attempt reduced the inbox-RDMA failures to one bindgen entry;
- the second exposed 13 KRdmaKit API differences;
- the third compiled the RDMA/KRdmaKit layer and exposed 36 Mitosis MM
  ABI/layout errors.

The remaining compile work is therefore the Linux 4.15 to RHEL/Rocky 5.14 MM
port: `vm_fault_t`, KABI-wrapped MM/VMA fields, `mm_walk_ops`, page-table
helpers, and TLB helpers. Existing research-tree changes must first be
recovered into an isolated worktree without modifying the preserved source.

## P2 safety gate

No kernel module has been loaded. Before the first single-guest test, the
build must provide:

- an exact guest-kernel vermagic match;
- no unresolved symbols;
- the source revision, build manifest, full log, and module SHA-256;
- explicit load, test, unload, and failure-recovery commands for one named
  test guest.

Host GPU, VF, ACS, IOMMU, storage, and network configuration are not part of
the single-guest CPU fork gate.
