# SLSM Stage 1 Two-Guest RC SST Fetch

Date: 2026-08-20

## Result

The minimal SLSM Stage 1 data path passed between two isolated Rocky Linux
Guests placed on separate physical hosts. The CN registered deterministic SST
bytes with its local `slsm.ko`; the SN established one RC connection, fetched
the descriptor's chunks with one-sided RDMA READ, and validated the complete
payload in both kernel and userspace.

This is a correctness gate, not a performance result or an end-to-end
ServerlessLSM reproduction.

## Source and environment

- Source branch: `feat/slsm-stage1-rc-poc-20260819`
- Mitosis-compatible base: `43b5335fe4a32080bfa6f715d3f4200a350d538b`
- Original validation dependency: `37de677` (Rocky/RoCE base used for the
  successful two-Guest run)
- Current hardened source dependency: `0b9c8ca` (the follow-up source with
  zeroed RC metadata padding; compiled below but not loaded or rerun)
- Guest OS: Rocky Linux 9.8
- Guest kernel: `5.14.0-687.10.1.el9_8.0.1.x86_64`
- Transport: RC over RoCEv2, port 1, GID index 3
- Device isolation: one passed-through RDMA VF and exactly one visible RDMA
  device per Guest

The kernel module was built against the exact running-kernel build tree. BTF
generation was skipped because the build Guest did not expose `vmlinux`.

## Validation matrix

| SST size | SST ID | Chunks | FNV-1a checksum | SN kernel fetch time | Result |
| ---: | ---: | ---: | --- | ---: | --- |
| 1 MiB | 1001 | 1 | `0x141541678c6aa325` | 5,760 us | CN/SN pass |
| 8 MiB | 1008 | 8 | `0x61fc2bb9fe662325` | 33,011 us | CN/SN pass |

The kernel fetch time covers this prototype's serialized RDMA READ, completion
polling, checksum, and copy-to-user loop. The run was not repeated or tuned,
so these values must not be presented as latency measurements or compared with
paper baselines.

The original successful run artifacts were:

```text
slsm.ko  ec467eb61cabe03c1f540c5d1ee2b8390744a1be352f1ce8ca03fefd4da7bb80
slsm_cn  eb4a75067acae5ecec294218e03672e2e422c9d6203a5144da4173d4c9dc0ca3
slsm_sn  7dcb7d1cd27372e9b5d5afbeb892531949b048558d3efaf6465620b4df1b77d1
```

The review-hardened source was subsequently compiled, without loading the
module or running either Guest. Its compile-only artifacts are:

```text
slsm.ko  232a03bb61e0f613c33172ba8aafc29ae0b7c3614a350e45534012e789ac9527
slsm_cn  56fd85a72f34741458e7ad67fff0cead1067e26b19239b1bd81bcb3a8f66cf07
slsm_sn  d13e6f08eb2af89fc2db832e41652a4999c1cc0ed0825724b66253bef6c158f7
```

The compile used the exact Guest kernel build tree and completed modpost,
vermagic, and dependency checks; BTF generation was skipped because the build
Guest did not expose `vmlinux`. Generated modules and executables are not
committed. The original hashes remain the only runtime-validated artifacts;
the hardened hashes establish compilation only until the correctness matrix is
rerun.

## Observed warnings

Both module loads emitted the inherited warnings:

```text
Unpatched return thunk in use. This should not happen!
rust-kernel-rdma-base: enabling unsafe global rkey
```

No Oops, BUG, panic, SSH loss, or test failure followed during this gate. The
warnings still prevent a production-readiness claim. In particular, the
global-rkey path and physical-address descriptors are acceptable only in these
isolated research Guests. The reviewed source now requires `CAP_SYS_RAWIO`,
allows only one open file, uses checked SLSM-owned MR allocations, zeroes the
inherited RC metadata padding, matches CQ completions by work-request ID, and
does not free a posted DMA target on a local timeout.

## Final state

For the original validation, the two test processes exited and no control
listener remained. Both test Guests kept the original `slsm.ko` loaded with
module reference count zero. The modules were not unloaded because the
inherited RC/CM server teardown can race retained sessions. The later
compile-only build Guest did not load the hardened module. Neither physical
host nor Guest was rebooted for either operation.

No active RDMA discovery or transfer was run on a physical host. Raw logs,
internal addresses, VM definitions, credentials, binaries, and private machine
inventory remain outside this repository.

## Claim boundary and next gate

The original result establishes only the following chain:

```text
CN userspace buffer
  -> kernel-owned chunk regions and descriptor
  -> one RC connection
  -> SN one-sided RDMA READ
  -> byte-pattern and checksum agreement
```

It does not include Nova-LSM, real SST files, storage I/O, compaction, rFork,
rMMap page faults, epochs, range locks, Manifest publication, persistence,
failure recovery, containers, resource elasticity, or a scheduler.

The next small-paper gate should add a minimal SST/epoch lifecycle on top of
this fixed transport contract before attempting Nova-LSM integration.
