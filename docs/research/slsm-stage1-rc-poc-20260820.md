# SLSM Stage 1 RC SST Fetch

Date: 2026-08-20

This is a sanitized record of external evidence from a separate Mitosis-based
worktree. It is not a ServerlessX implementation release and does not make the
external source, kernel module, VM images, or generated binaries part of this
repository.

## Result

The original, pre-hardening prototype passed a narrow correctness gate between
two isolated Rocky Linux Guests on separate physical hosts. A compute node (CN)
published a deterministic, SST-shaped in-memory byte buffer; a storage node
(SN) established one RC connection, fetched the chunks with one-sided RDMA READ,
and agreed on the payload checksum in kernel and userspace.

This is a mechanism and correctness gate only. It is not a performance result,
an end-to-end ServerlessLSM reproduction, or evidence that the ServerlessX
quickstart can run an SLSM workload.

## Tested artifact and hardened source

The successful run used the external KRCore/RoCE base at `37de677`. Its
validation covered the exact Guest kernel `5.14.0-687.10.1.el9_8.0.1.x86_64`,
RC over RoCEv2, port 1, IPv4-mapped GID index 3, and one passed-through RDMA VF
visible in each Guest.

The original successful artifact SHA-256 values were:

```text
slsm.ko  ec467eb61cabe03c1f540c5d1ee2b8390744a1be352f1ce8ca03fefd4da7bb80
slsm_cn  eb4a75067acae5ecec294218e03672e2e422c9d6203a5144da4173d4c9dc0ca3
slsm_sn  7dcb7d1cd27372e9b5d5afbeb892531949b048558d3efaf6465620b4df1b77d1
```

After that run, the source was reviewed and hardened on top of KRCore revision
`0b9c8ca`. The review changes include capability checks, single-open device
ownership, checked SLSM-owned MR allocation, completion matching by work-request
ID, removal of a timeout that could free a live DMA target, explicit RC metadata
padding initialization, and required peer-address arguments in userspace.

The hardened source was subsequently compiled against the exact Guest kernel,
but it was not loaded or correctness-rerun in either test Guest. The
compile-only artifact hashes are:

```text
slsm.ko  232a03bb61e0f613c33172ba8aafc29ae0b7c3614a350e45534012e789ac9527
slsm_cn  56fd85a72f34741458e7ad67fff0cead1067e26b19239b1bd81bcb3a8f66cf07
slsm_sn  d13e6f08eb2af89fc2db832e41652a4999c1cc0ed0825724b66253bef6c158f7
```

The original module remains the only runtime-validated kernel artifact; the
hardened tree is compile-verified but must not be described as runtime-tested
until the correctness matrix is rerun.

## Validation matrix

| SST-shaped payload | Chunks | Prototype kernel fetch time | Result |
| ---: | ---: | ---: | --- |
| 1 MiB | 1 | 5,760 us | CN/SN checksum agreement |
| 8 MiB | 8 | 33,011 us | CN/SN checksum agreement |

The prototype times include serialized RDMA READ, completion polling, checksum,
and copy-to-user. The runs were not repeated or tuned; these values must not be
presented as latency or throughput measurements or compared with paper baselines.

## Minimal lifecycle follow-up

The external userspace control protocol was then extended with a small,
explicit lifecycle:

```text
PUBLISH -> FETCH -> COMMIT -> REVOKE
```

Using the original module still loaded in both Guests, the lifecycle clients
passed checksum validation and completed all four phases for both payload sizes:

| Direction | Payload | Chunks | FNV-1a checksum | SN fetch time | Result |
| --- | ---: | ---: | --- | ---: | --- |
| mem01 CN -> mem02 SN | 1 MiB | 1 | `0xb6eae9c0f46aa325` | 5,770 us | pass |
| mem02 CN -> mem01 SN | 8 MiB | 8 | `0x9ccbaf967e662325` | 32,947 us | pass |

The 8 MiB reverse-direction run was used because the inherited CM/RC service
returned `ECONNREFUSED` for a second session in the original direction after
teardown. No module was unloaded and no Guest was rebooted to reset that state.
This is a lifecycle correctness check, not a directional performance result.

The original module loads emitted inherited return-thunk and unsafe-global-rkey
warnings. No Oops, BUG, panic, or test failure followed during this gate. The
physical-address descriptors and global-rkey path still restrict the prototype
to isolated research Guests.

## Claim boundary

The evidence establishes only this chain:

```text
CN userspace buffer
  -> kernel-owned chunk regions and descriptor
  -> one RC connection
  -> SN one-sided RDMA READ
  -> checksum agreement
```

It does not include Nova-LSM, a real on-disk SST format, storage I/O, flush,
compaction, RDMA mmap page faults, persistent epochs, range locks, Manifest
publication, persistence, recovery, function lifecycle, multi-tenancy,
elasticity, or a scheduler. COMMIT and REVOKE are protocol acknowledgements;
they do not make bytes durable or implement an LSM Manifest.

Raw logs, internal addresses, credentials, VM definitions, modules, and
executables remain outside this repository. No host or Guest reboot is part of
the recorded gate.

## Next gate

Load the compile-verified hardened source in the isolated test Guests and rerun
the transport and lifecycle matrices in a controlled module-lifecycle window.
Only after that result is recorded should SST/epoch metadata or Nova-LSM
integration be attempted; performance work remains a later stage.
