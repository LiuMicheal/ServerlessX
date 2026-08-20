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

## Minimal SST/epoch/Manifest semantics

The next small-paper layer keeps the kernel descriptor ABI unchanged and adds
only user-space metadata to the lifecycle message. The wire protocol was
bumped to lifecycle version 2 because the message and acknowledgement now carry
the following fields:

```text
SST metadata = { level, epoch, inclusive min_key, inclusive max_key }
SN Manifest  = { pending | committed entries, version }
```

The healthy-session state transition is:

```text
PUBLISH  -> pending Manifest entry
FETCH    -> checksum agreement for the descriptor bytes
COMMIT   -> committed entry visible to epoch/key-range lookup
REVOKE   -> entry removed; lookup may fall back to another committed entry
```

The Manifest implementation is deliberately process-local and bounded to eight
entries. It is metadata routing, not a real key/value lookup: the test payload
is still a deterministic byte pattern and the lookup checks only the declared
key range, epoch, and LSM level priority. A standalone test covers two
overlapping SSTs, pending invisibility, epoch/state checks, L0 priority, and
fallback after revoke:

```text
{"event":"manifest_test","status":"pass","entries":2,
 "lookup_key":75,"final_count":0,"version":6}
```

A pre-v2 live 1 MiB smoke using the original loaded kernel module exercised the
new metadata path and produced:

```text
CN: {"event":"stage3_result","status":"pass","sst_id":4001,
     "level":0,"epoch":7,"key_range":[4001000,4001999],
     "manifest_version":3}
SN: {"event":"stage3_result","status":"pass","sst_id":4001,
     "manifest_version":3}
```

The final v2 userspace rebuild was copied to both Guests. A reverse-direction
8 MiB session then passed with the new wire format:

```text
CN: {"event":"stage3_result","status":"pass","sst_id":4008,
     "level":0,"epoch":8,"key_range":[4008000,4008999],
     "manifest_version":3}
SN: {"event":"stage3_result","status":"pass","sst_id":4008,
     "bytes":8388608,"elapsed_us":33116,"manifest_version":3}
```

The direction was reversed because the inherited CM/RC service does not
reliably accept a second session after teardown. No module was unloaded and no
Guest was rebooted.

The final userspace artifact hashes are:

```text
slsm_cn             f8868cf985496d52975a3b0826728c37ffa40fab16b77805179dd976550acadb
slsm_sn             9fb2246d2ec32c59001399b3ffd5f400268efd3f5edfd86c6550d39a39b18996
slsm_manifest_test  3cd0b35afcf6bc93e7b9e9b534f42e3ba7cf7cd96fae3bc9bd3d54a257d6da50
```

## Stage 4: real SST and MemTable flush/read

The next small-paper gate replaced the deterministic byte pattern with a small
real, in-memory SST format while keeping the kernel descriptor and RDMA ABI
unchanged. The format has a fixed header, sorted `uint64_t` key/value records,
and a 32-record bound. CN inserts records into a sorted MemTable and flushes it
to an SST before registering the exact encoded length. SN validates the header,
SST ID, epoch, key range, record ordering, and then performs one binary-search
point lookup after the RDMA READ. The Manifest lifecycle remains
`PUBLISH -> FETCH -> COMMIT -> REVOKE`.

The standalone tests passed:

```text
{"event":"manifest_test","status":"pass","entries":2,
 "lookup_key":75,"final_count":0,"version":6}
{"event":"sst_test","status":"pass","records":3,"bytes":104,
 "lookup_key":20,"lookup_value":222}
```

Two single-session Guest checks passed with the original loaded module. In the
first, mem02 acted as CN and mem01 as SN: eight records (184 bytes) were read,
the checksum was `0xb0127df7a653a348`, and the SN reported `9 us`. In the
second, mem01 acted as CN and mem02 as SN: five records (136 bytes) were read,
the checksum was `0xe1782d3411145d10`, and the SN reported `8 us`. Both roles
reported `stage4_result=pass`, and the point lookups returned the expected
`key + 7` values. These one-shot times are correctness-run observations, not
performance measurements.

The current userspace artifact hashes are:

```text
slsm_cn             967b60e11510ef3d192f477169d8cb0071122f4db626aa112f094ff64dbfe6c8
slsm_sn             fef6774d7a572e2e97cbb66922c52f06a9a6ec0008397dd42808a8b1b0c5827f
slsm_manifest_test  3cd0b35afcf6bc93e7b9e9b534f42e3ba7cf7cd96fae3bc93bd3d54a257d6da50
slsm_sst_test       ed8174882a5c0fb84bf85463924ce2459d88dbf03e64c8ef649bab2e71b8c330
```

No kernel module was replaced or unloaded, and no host or Guest was rebooted.

## Control-plane hardening (offline only)

After the Stage 4 Guest checks, the userspace control path was tightened
without changing the kernel descriptor or RDMA ABI. Framed TCP reads and writes
now use an absolute 60-second deadline, so a peer that keeps a connection open
without completing a lifecycle message cannot retain the userspace session
indefinitely. The SN removes the session's pending or committed Manifest entry
on every failed post-PUBLISH path. For the successful REVOKE exchange it
disconnects the local QP before sending the final ACK, and the CN reports pass
only after `UNREGISTER_REGION` succeeds. CN generations are nonzero and vary
per run; the SN rejects a zero generation.

The changes were compiled and tested offline only. No module was loaded, no
Guest session was rerun, and no physical host or RNIC operation was performed.

```text
{"event":"common_test","status":"pass","timeout_ms":20}
{"event":"manifest_test","status":"pass","entries":2,
 "lookup_key":75,"final_count":0,"version":8}
{"event":"sst_test","status":"pass","records":3,"bytes":104,
 "lookup_key":20,"lookup_value":222}
```

The compile-only userspace hashes are listed below after the build. These are
source-build evidence only; the earlier Guest-run hashes in the Stage 4 section
remain the only runtime-validated artifacts.

```text
slsm_cn             469d8957f9d01ab8b4f00cdd98a2581db6e1652ed25ec54cffbbc9d0a7084a28
slsm_sn             931b92c31b6d3a7151c9d97f66559db1719c808d0ac5c2adc9aa223bf1df28f6
slsm_common_test    f2f234a0c62aa2721119df0a336f934188f1e21e58b7182ed44d3490c20f5568
slsm_manifest_test  973eadc30218554db21c37cb1e471bef7410ec5f002d160a28d81ddb0dcb0029
slsm_sst_test       ed8174882a5c0fb84bf85463924ce2459d88dbf03e64c8ef649bab2e71b8c330
```

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

The combined result establishes only the following chain:

```text
CN userspace buffer
  -> kernel-owned chunk regions and descriptor
  -> one RC connection
  -> SN one-sided RDMA READ
  -> real SST validation, checksum agreement, and one point lookup
```

It does not include Nova-LSM, storage I/O, compaction, rFork, rMMap page faults,
persistent epochs, range locks, persistence, failure recovery, containers,
resource elasticity, or a scheduler. The SST and Manifest are user-space and
process-local; this is one MemTable flush/read path, not a complete durable LSM.

The next gate is to measure repeated workloads and then decide whether a small
Nova-LSM-style read API is needed. Full Nova-LSM integration, persistence, and
compaction remain later work.
