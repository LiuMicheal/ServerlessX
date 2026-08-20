# ServerlessLSM (SLSM)

ServerlessLSM is the I/O-intensive storage system in the ServerlessX research
architecture. It applies RDMA mmap to disaggregated LSM-tree storage, separates
foreground access from independently scalable flush and compaction functions,
and coordinates metadata without a centralized lock manager.

This development snapshot still contains no bundled ServerlessLSM
implementation, RDMA mmap runtime, runnable profile, kernel module, VM image, or
evidence that reproduces the paper's performance claims. A separate
Mitosis-based worktree has produced a small external Stage 1 mechanism
prototype; its sanitized evidence note is [SLSM Stage 1 RC SST
Fetch](../../docs/research/slsm-stage1-rc-poc-20260820.md). That note does not
make the prototype executable through ServerlessX.

The external follow-up adds only user-space `level`/`epoch`/key-range metadata
and a bounded pending/committed Manifest. It remains metadata-routing evidence:
the payload is a deterministic byte pattern, the Manifest is process-local, and
no real SST reader or LSM workload is bundled.

The original prototype passed a two-Guest RC SST-fetch correctness gate for
1 MiB and 8 MiB SST-shaped in-memory payloads. The post-run hardened source has
now been compile-verified against the exact Guest kernel, but it was not loaded
or rerun, so runtime evidence still applies only to the original external
artifact. There is no SLSM performance, storage-I/O, compaction, RDMA-mmap, or
end-to-end claim.

Future source must define the mapping and metadata contracts, storage ownership,
failure and recovery behavior, provenance, and verification evidence before the
subsystem is advertised as executable.
