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

## Minimal SST/epoch/Manifest follow-up

The external userspace prototype then added lifecycle protocol version 2
metadata without changing the kernel descriptor ABI:

```text
SST metadata = { level, epoch, inclusive min_key, inclusive max_key }
SN Manifest  = { pending | committed entries, version }
```

On a healthy session, `PUBLISH` creates a pending Manifest entry, `COMMIT`
makes it visible to an epoch/key-range metadata lookup, and `REVOKE` removes it
or permits lookup to fall back to another committed entry. The Manifest is
process-local, bounded, and user-space only.

The standalone two-SST test passed pending invisibility, epoch/state checks,
same-epoch L0 priority, and fallback after revoke:

```text
{"event":"manifest_test","status":"pass","entries":2,
 "lookup_key":75,"final_count":0,"version":6}
```

A pre-v2 live 1 MiB metadata smoke also passed with `sst_id=4001`, `level=0`,
`epoch=7`, key range `[4001000,4001999]`, and final Manifest version `3` on
both roles. The final v2 userspace binaries compile-verified and were copied to
both Guests. A reverse-direction 8 MiB session then passed with `sst_id=4008`,
`level=0`, `epoch=8`, key range `[4008000,4008999]`, Manifest version `3`, and
SN fetch time `33,116 us`. The direction was reversed because the inherited
CM/RC service does not reliably accept a second session after teardown. The
lookup is metadata routing only: the payload remains a deterministic byte
pattern, not a parsed SST key/value table.

Final userspace artifact hashes:

```text
slsm_cn             f8868cf985496d52975a3b0826728c37ffa40fab16b77805179dd976550acadb
slsm_sn             9fb2246d2ec32c59001399b3ffd5f400268efd3f5edfd86c6550d39a39b18996
slsm_manifest_test  3cd0b35afcf6bc93e7b9e9b534f42e3ba7cf7cd96fae3bc9bd3d54a257d6da50
```

## Stage 4: real SST and MemTable flush/read

The external prototype then replaced the deterministic byte pattern with a
small in-memory SST format without changing the kernel descriptor or RDMA ABI.
The format uses a fixed header and sorted `uint64_t` key/value records, bounded
to 32 records. CN fills a sorted MemTable and flushes it to an SST before
registering the exact encoded length. SN validates the SST header, ID, epoch,
range, and ordering, then performs one binary-search point lookup after the
RDMA READ. The existing `PUBLISH -> FETCH -> COMMIT -> REVOKE` lifecycle is
unchanged.

The standalone Manifest and SST tests passed. Two single-session Guest checks
also passed: one with eight records (184 bytes, SN observation `9 us`) and one
with five records (136 bytes, SN observation `8 us`). Checksums agreed in both
directions, and both point lookups returned the expected values. These are
correctness-run observations, not performance measurements. No module was
replaced or unloaded, and no host or Guest was rebooted.

This adds one real user-space MemTable flush/read path to the external evidence,
but it remains process-local and in-memory. It is not a durable Nova-LSM,
storage benchmark, compaction implementation, or end-to-end ServerlessLSM
system.

The original module loads emitted inherited return-thunk and unsafe-global-rkey
warnings. No Oops, BUG, panic, or test failure followed during this gate. The
physical-address descriptors and global-rkey path still restrict the prototype
to isolated research Guests.

## Claim boundary

The combined evidence establishes only this chain:

```text
CN userspace buffer
  -> kernel-owned chunk regions and descriptor
  -> one RC connection
  -> SN one-sided RDMA READ
  -> real SST validation, checksum agreement, and one point lookup
```

It does not include Nova-LSM, storage I/O, compaction, RDMA mmap page faults,
persistent epochs, range locks, durable Manifest publication, persistence,
recovery, function lifecycle, multi-tenancy, elasticity, or a scheduler. The
SST and Manifest are user-space and process-local, so this is not a complete
durable LSM read path.

Raw logs, internal addresses, credentials, VM definitions, modules, and
executables remain outside this repository. No host or Guest reboot is part of
the recorded gate.

## Nova-compatible SST adapter (offline follow-up)

The Nova-LSM format audit used a direct clone of
`https://github.com/HaoyuHuang/NovaLSM` at commit
`8a661197ce5b993f2baeef608f34192d1ef0adf5` (2021-06-20). The complete Nova
database was not copied into this repository and is not linked into the small C
prototype because its DB API depends on Nova configuration, remote storage,
RDMA, Manifest, and compaction services.

The external Mitosis worktree now contains a dependency-free, clean-room
adapter for the format semantics used by Nova's LevelDB-derived table reader:
internal keys, prefix-compressed data blocks, restart metadata, index and empty
metaindex blocks, masked CRC32C trailers, the 48-byte footer, and snapshot-aware
point lookup. It wraps that payload in the existing SLSM header, so the kernel
descriptor ABI and `PUBLISH -> FETCH -> COMMIT -> REVOKE` lifecycle are
unchanged.

The adapter's offline tests passed (four entries including two sequence
versions, CRC corruption rejection, and outer SLSM lookup):

```text
{"event":"nova_sst_test","status":"pass","entries":32,"bytes":767,"lookup_sequence":2}
{"event":"sst_test","status":"pass","format":"nova-leveldb-table","records":3,"bytes":231,"lookup_key":20,"lookup_value":222}
```

This demonstrates Nova-compatible SST/lookup semantics over the existing SLSM
data path; it does not reproduce Nova's full DB, storage service, compaction,
or performance behavior. These results are source-build/offline only: no
Guest session, module load, Host reboot, or physical RNIC operation was used.

## Next gate

Measure repeated small-SST workloads and then decide whether a small
Nova-LSM-style read API is needed. Full Nova-LSM integration, persistence, and
compaction remain later work.

## Nova SST Guest correctness gate (2026-08-21)

The external Mitosis worktree rebuilt the two userspace clients with
`make -C slsm-user clean test`; all four offline tests passed. The new client
artifact hashes were:

```text
slsm_cn  17889db1eaf64c525551099d91b6ece9ca6abbb5886d99a8c32701372e709e5b
slsm_sn  7b708b60c7a6eb6bb80c8cc7ce4411e48e3d0d2e5f8ed13e5c74d984ea3408bf
```

The first non-privileged attempt was rejected by the existing module at
`open("/dev/slsm")` with `EPERM`; the module explicitly requires
`CAP_SYS_RAWIO`. A bounded retry used Guest-root privileges only, without
changing the module or rebooting either Guest or Host.

With the compute role on mem01 and storage role on mem02, the retry passed:

```text
{"event":"stage4_result","role":"cn","status":"pass",
 "lifecycle":"publish-fetch-commit-revoke","sst_id":20260822,
 "bytes":332,"chunks":1,"checksum":"0xbd0eb86d4d7c34de",
 "sn_elapsed_us":11,"records":8,"manifest_version":3}
{"event":"stage4_result","role":"sn","status":"pass",
 "lifecycle":"publish-fetch-commit-revoke","sst_id":20260822,
 "bytes":332,"chunks":1,"checksum":"0xbd0eb86d4d7c34de",
 "elapsed_us":11,"lookup_key":20260822000,
 "lookup_value":20260822007,"manifest_version":3}
```

The same checksum, Nova footer/block CRC validation, point lookup, and
`PUBLISH -> FETCH -> COMMIT -> REVOKE` Manifest lifecycle were observed on both
roles. Both processes exited and the listener was released. This is a single
correctness run, not a performance measurement; the `11 us` observation must
not be compared with baselines. The original module remained loaded and no
module unload or reboot occurred.

This is runtime evidence for the small Nova-compatible SST adapter over the
existing SLSM RC/RDMA path. It is not evidence of a complete Nova-LSM engine,
persistent storage, compaction, or ServerlessX integration. Repeated small-SST
runs remain the next scoped experiment.

## Nova-LSM original build and local workload (2026-08-21)

The upstream Nova-LSM source was cloned directly in the mem01 Guest from
`https://github.com/HaoyuHuang/NovaLSM` at commit
`8a661197ce5b993f2baeef608f34192d1ef0adf5` (2021-06-20). The clone remains
outside this repository at `/home/liumx/nova-lsm-baseline-20260821`; mem02 was
not modified or used for this build.

Using the repository CMake targets and a user-prefix for static `gflags` and
`fmt`, the Guest build completed with:

```text
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS='-I/home/liumx/nova-deps/prefix/include -U__SSE4_2__'
cmake --build build -j2
```

The five compatibility edits were local to the Guest clone (standard/math
headers and one Nova opcode-formatter qualification); no upstream source was
changed or pushed. SHA-256 values of the generated targets are:

```text
nova_server_main        17506425ce4ecc63f207b7a69e51a22aa17f3b850c8dea79076d82a7f6da5619
nova_server_main_debug  583de668a1290e77483216caa8c041ff2a57488ed02bc6e8946346e98f88cd12
nova_subrange_sim       dd4486b18f64980b52cfffbc925abfe6beedeb369e2c19afc4b130a2280cfdbc
file_reader             7ab9821149f522cdf235028698f2bb3aadb28923e268b3c33b123dbb4486cf6a
scatter_bench           b4ffc385828d75b6d9fe13c68ed8e96dbce14d449c8d3851d9b7e08fbe2708e7
memtable_bench          49fd74f7d1c44f00ff456ba5efed15176d2d2f69c8cec458615773c65de3e923
version_set_test        3a5ca17349576fe86cbc134faf376306b97a8c5de6a14910f9eec7f7e9949dc1
bloom_test              23dfa8c9675806a96a68e2cdbdb4330f93076e8c3d7250f4ff7bae5a60456d93
filter_block_test       d1735e563c7ead711c55a16c990c41b4f0293e635b024f56f3a19af3c779dafe
```

`bloom_test` (3 tests) and `filter_block_test` (2 tests) passed. The upstream
`version_set_test` terminated with SIGSEGV (exit 139) in its first
`TestNonOverlappingSetDEBUG3` case; no broad fix was attempted. A local
MemTable smoke workload using two workers, two partitions, a 1 MiB MemTable,
1,000 keys, 32-byte values, and 4,000,000 inserts per worker completed with:

```text
memtable_bench --num_workers=2 --nkeys=1000 --value_size=32 \
  --memtable_size_mb=1 --npartitions=2 --max_ops=4000000
throughput,1333332.0
```

This is process-local and in-memory, and the output is only a build/workload
smoke observation. The full `nova_server_main` was intentionally not started:
it requires Nova's multi-node configuration and initializes the RDMA control
plane, which is outside this minimal local gate. No physical-host RDMA command,
Host or Guest reboot, module change, or storage benchmark was performed. This
record therefore confirms that the original code can be built and its local
MemTable path exercised in mem01, not that Nova-LSM is integrated into
ServerlessX or reproduced end to end.
