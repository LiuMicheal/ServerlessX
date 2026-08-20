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

## Minimal lifecycle validation

The follow-up userspace control protocol added a deliberately small lifecycle:

```text
PUBLISH -> FETCH -> COMMIT -> REVOKE
```

The new userspace clients were run with the original runtime-validated module
(`ec467eb6...`) still loaded in both Guests. The lifecycle change is control
plane only; it uses the existing register, RC connect, RDMA READ, disconnect,
and unregister ioctls. The clients were run with `CAP_SYS_RAWIO` inside the
isolated Guests.

| Direction | Payload | SST ID | Chunks | FNV-1a checksum | SN fetch time | Result |
| --- | ---: | ---: | ---: | --- | ---: | --- |
| mem01 CN -> mem02 SN | 1 MiB | 2001 | 1 | `0xb6eae9c0f46aa325` | 5,770 us | lifecycle pass |
| mem02 CN -> mem01 SN | 8 MiB | 3008 | 8 | `0x9ccbaf967e662325` | 32,947 us | lifecycle pass |

The lifecycle clients were:

```text
slsm_cn  32df61f4ac8976e131208308f51e6681833ed0dc2b25d2800b1424e5cae3cc24
slsm_sn  d6a31bbaf5aa91fde60ce3c16e2185cbb99aa0eb2815e291d89bacc529b8b886
```

An additional 8 MiB attempt in the original mem01-CN/mem02-SN direction was
rejected at `CONNECT_PEER` with `ECONNREFUSED` after the first lifecycle run.
The inherited CM/RC implementation does not reliably accept a second session
after teardown without a module lifecycle reset. We did not unload a module or
reboot a Guest to force that reset. The reverse-direction pass therefore
demonstrates the multi-chunk lifecycle contract, but is not a directional
performance comparison.

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

For both validation passes, the test processes exited and no control listener
remained. Both test Guests kept the original `slsm.ko` loaded with module
reference count zero. The modules were not unloaded because the inherited
RC/CM server teardown can race retained sessions and does not reliably support
a second session. The later compile-only build Guest did not load the hardened
module. Neither physical host nor Guest was rebooted for either operation.

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
rMMap page faults, persistent epochs, range locks, Manifest publication,
persistence, failure recovery, containers, resource elasticity, or a scheduler.
The new COMMIT and REVOKE messages are protocol acknowledgements only; they do
not make the fetched bytes durable or implement an LSM Manifest.

The next small-paper gate is to rerun this lifecycle with the hardened module
in a controlled module-lifecycle window, then add only the minimum SST/epoch
metadata needed for an LSM claim before attempting Nova-LSM integration.
