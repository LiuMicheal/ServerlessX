# PhOS lab safety rules

These rules add to the repository-level `AGENTS.md` whenever `phos_lab` is
selected.

- Treat this as an operator-controlled lab profile, never as a portable
  fallback.
- Begin with read-only inventory: host identity, GPU UUIDs, driver/toolkit
  versions, RDMA devices and gids, VM definitions, daemon PIDs/start ticks,
  module hashes, and available disk. Record the inventory outside this public
  bootstrap when it contains private topology.
- Do not start, stop, clone, reset, suspend, or mutate a VM; load/unload a
  kernel module; change VF/IOMMU/RDMA state; or alter shared daemon/container
  state without explicit approval naming the target and operation.
- Do not assume a running SPD VM belongs to the current run. Never terminate a
  process by a broad name or delete a shared image, SHM segment, checkpoint, or
  cache.
- Use an explicit run ID, exact source and target identities, ownership ledger,
  timeout, and rollback/cleanup plan before any approved operation.
- Keep external source and generated binaries in their original controlled
  worktrees or artifact stores. Do not copy them into this repository.
- Stop on an unexpected PID, start tick, hash, endpoint, device, VM state, or
  ABI. Preserve the failed evidence and report it; do not repair state in place.
- A successful vector checksum or one RDMA read is not by itself evidence of a
  complete model migration, production readiness, or public reproducibility.
