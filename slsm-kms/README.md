# SLSM Stage 1 RC SST Fetch

This directory contains the first mechanism-only ServerlessLSM prototype. It
establishes one narrow contract: a compute node (CN) can publish an in-memory
SST descriptor and a storage node (SN) can fetch the SST through one-sided RC
RDMA READs.

It is not Nova-LSM integration and it does not implement flush, compaction,
`mmap`, epochs, range locks, Manifest updates, persistence, recovery, function
lifecycle management, or scheduling.

## Data path

1. `slsm_cn` fills a deterministic SST-shaped byte buffer and registers it
   through `/dev/slsm`.
2. `slsm.ko` copies the buffer into up to eight 1 MiB kernel memory regions and
   returns a versioned descriptor containing the owner GID, remote addresses,
   lengths, rkeys, and checksum.
3. The CN sends that descriptor to `slsm_sn` over a small TCP control channel.
4. The SN asks its local `slsm.ko` to create one RC connection and fetch each
   chunk with RDMA READ.
5. Kernel and userspace independently verify an FNV-1a checksum and the SN
   returns an acknowledgement to the CN.

The control channel carries metadata only. SST bytes move through RDMA.

## Layout

```text
slsm-kms/
  Kbuild                         kernel build rule
  Makefile                       external-module build entry point
  slsm/src/lib.rs                /dev/slsm and the five ioctl operations
slsm-user/
  include/slsm_uapi.h            shared ABI and descriptor definitions
  slsm_cn.c                      region owner and control listener
  slsm_sn.c                      RC client, RDMA reader, and validator
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

- x86-64 homogeneous peers and ABI version 1;
- exactly one RDMA device visible inside each isolated Guest;
- RDMA port 1 and IPv4 RoCEv2 GID index 3;
- one privileged `/dev/slsm` open, one RC session, and one in-flight read at a
  time;
- 1-8 chunks of at most 1 MiB each, for an 8 MiB maximum SST;
- a trusted, single-purpose Guest environment.

After an operator has separately loaded the matching module in both Guests,
the userspace gate can be run with documentation-only addresses as follows:

```bash
# CN Guest
./slsm-user/bin/slsm_cn \
  --bind 192.0.2.10 \
  --size 1048576 \
  --sst-id 1001

# SN Guest
./slsm-user/bin/slsm_sn --server 192.0.2.10
```

Replace the example address with the CN address reachable from the SN. The CN
requires `--bind`, and the SN requires `--server`. Because `/dev/slsm` exposes
an unsafe global-rkey research path, both programs require `CAP_SYS_RAWIO`
(normally root). Success requires a `stage1_result` JSON line with `status` set
to `pass` from both roles.

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
