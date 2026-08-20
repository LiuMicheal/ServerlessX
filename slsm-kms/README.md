# SLSM Stage 1 RC SST Fetch

This directory contains the first mechanism-only ServerlessLSM prototype. It
establishes a narrow contract: a compute node (CN) can flush a small in-memory
MemTable into an SST, publish its descriptor, and a storage node (SN) can fetch
the SST through one-sided RC RDMA READs, validate it, and perform a point lookup
before exposing it through a small user-space Manifest.

It is not Nova-LSM integration and it does not implement storage I/O,
compaction, `mmap`, range locks, persistence, recovery, function lifecycle
management, or scheduling. The SST and Manifest are in-memory and user-space
only.

## Data path

1. `slsm_cn` inserts a small deterministic set of key/value pairs into a
   MemTable, flushes it into an SST, and registers the exact encoded bytes
   through `/dev/slsm`.
2. `slsm.ko` copies the buffer into up to eight 1 MiB kernel memory regions and
   returns a versioned descriptor containing the owner GID, remote addresses,
   lengths, rkeys, and checksum.
3. The CN sends that descriptor and its LSM metadata (`level`, `epoch`, and key
   range) to `slsm_sn` over a small TCP control channel.
4. The SN asks its local `slsm.ko` to create one RC connection and fetch each
   chunk with RDMA READ.
5. Kernel and userspace independently verify an FNV-1a checksum and the SN
   returns a FETCH acknowledgement to the CN.
6. The SN inserts the SST as pending in its in-memory Manifest; `COMMIT` makes
   it visible to an epoch/key lookup, and `REVOKE` removes that visibility.

The control channel carries metadata only. SST bytes move through RDMA.

## Layout

```text
slsm-kms/
  Kbuild                         kernel build rule
  Makefile                       external-module build entry point
  slsm/src/lib.rs                /dev/slsm and the five ioctl operations
slsm-user/
  include/slsm_uapi.h            shared ABI and descriptor definitions
  slsm_cn.c                      MemTable flush, region owner, and listener
  slsm_sn.c                      RC client, RDMA reader, SST validator, and lookup
  slsm_sst.[ch]                  bounded SST and MemTable implementation
  slsm_sst_test.c                 offline SST flush/validate/lookup test
  slsm_manifest.[ch]             minimal in-memory SST Manifest
  slsm_manifest_test.c            offline Manifest/lookup test
```

The five operations are `CONNECT_PEER`, `REGISTER_REGION`, `FETCH_REGION`,
`UNREGISTER_REGION`, and `DISCONNECT`. State is intentionally per open file.

## Build

The kernel module inherits the Mitosis/KRdmaKit Rust-for-Linux build and its
submodules. The current hardened KRCore source pin (`0b9c8ca`) explicitly
zeros the padding in its RC connection metadata before that metadata crosses
the wire. The documented runtime pass used its parent (`37de677`); the
hardened source has since been compiled against the exact Guest kernel but has
not been loaded or rerun. Build only against the headers and compiler for the
exact Guest kernel that will load it:

```bash
make -C slsm-kms \
  KDIR=/lib/modules/$(uname -r)/build \
  CC=gcc
make -C slsm-user
```

For an already-vendored offline Cargo environment,
`cargo-config.offline.example.toml` is a template for the repository-root
`.cargo/config.toml`. Its relative path expects the vendored crates under
`vendor/`.

The original runtime-validated target was
`5.14.0-687.10.1.el9_8.0.1.x86_64`; the hardened source also compiled against
that exact target. Other kernels, RDMA stacks, or compiler versions are
untested. See the experiment record for the separate runtime and compile-only
hashes.

## Run boundary

This PoC has deliberately fixed assumptions:

- x86-64 homogeneous peers, kernel descriptor ABI version 1, and lifecycle
  metadata protocol version 2;
- exactly one RDMA device visible inside each isolated Guest;
- RDMA port 1 and IPv4 RoCEv2 GID index 3;
- one privileged `/dev/slsm` open, one RC session, and one in-flight read at a
  time;
- 1-8 chunks of at most 1 MiB each, for an 8 MiB maximum SST;
- a trusted, single-purpose Guest environment;
- at most eight in-memory Manifest entries and no persistence.

After an operator has separately loaded the matching module in both Guests,
the userspace gate can be run with documentation-only addresses as follows:

```bash
# CN Guest
./slsm-user/bin/slsm_cn \
  --bind 192.0.2.10 \
  --records 8 \
  --epoch 1 \
  --sst-id 1001

# SN Guest
./slsm-user/bin/slsm_sn --server 192.0.2.10
```

Replace the example address with the CN address reachable from the SN. The CN
requires `--bind`, and the SN requires `--server`; `--records` accepts 1-32.
Because `/dev/slsm` exposes an unsafe global-rkey research path, both programs
require `CAP_SYS_RAWIO` (normally root). Success requires a `stage4_result`
JSON line with `status` set to `pass` from both roles. The result includes the
SST record count, point-lookup result, Manifest version, and metadata. `make -C
slsm-user test` runs both the Manifest and SST offline tests.

## Safety and limitations

KRdmaKit in this branch uses `IB_PD_UNSAFE_GLOBAL_RKEY` and exposes physical
addresses in descriptors. The module checks `CAP_SYS_RAWIO` on open and every
ioctl and permits only one open file, but the wire protocol still has no
authentication or tenant isolation. Do not use this prototype on a shared or
untrusted system.

SLSM-owned MR buffers use a checked kernel allocation rather than KRdmaKit's
infallible `MemoryRegion::new()` allocation. Once an RDMA READ has been posted,
the ioctl deliberately waits for the matching work-request completion before
freeing the staging buffer; no timeout path can free a live DMA target. A
wedged RNIC can therefore leave the privileged caller blocked. Proper QP
cancellation and CQ draining belong in the next transport-hardening stage.

The inherited RC/CM teardown can race retained sessions. Stop the exact test
processes and verify that no session remains before considering any module
lifecycle operation. This branch intentionally provides no automated module
load, unload, VM, Host, RNIC, or network management.

See
[`docs/experiments/slsm-stage1-rc-poc-20260820.md`](../docs/experiments/slsm-stage1-rc-poc-20260820.md)
for the validated two-Guest result and its claim boundary.
