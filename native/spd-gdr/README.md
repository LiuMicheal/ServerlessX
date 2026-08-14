# SPD native GDR data plane

This directory contains the ServerlessPD-owned native CUDA/RDMA boundary. The
default build selects `SPD_GDR_DIRECT_ONLY`: CUDA VMM memory is exported as a
DMA-BUF, registered with `ibv_reg_dmabuf_mr`, and transferred through a native
RC queue pair. TCP carries only bounded QP/MR metadata.

```bash
make spd-gdr
```

The build requires Linux, a C++17 compiler, CUDA headers and libraries, and
libibverbs headers and libraries. It compiles the shared library and two
hardware probes; it does not run a transfer, modify the host, or prove that the
driver/RNIC combination supports CUDA DMA-BUF.

The source retains an optional Mooncake implementation behind the inverse of
`SPD_GDR_DIRECT_ONLY`, but this repository does not vendor or build Mooncake.
The portable build therefore has no Mooncake dependency.

The four imported C++ files carry their existing Apache-2.0 SPDX identifiers.
Exact source commits and bootstrap path changes are recorded in
`provenance/included-code.json`.

## One-allocation GPU remote-fork canary

`phos_gpu_rfork_canary` is a private-lab probe that composes the owned rfork
userspace boundary with a separately supplied PhOS C ABI. It creates exactly
one 2 MiB GPU-Direct-capable CUDA VMM allocation, prepares a caller-owned PhOS
memory image, resumes into a remote child, requires clone-attach before the
child's first CUDA call, and verifies both the restored digest and a
target-local mutation.

```bash
make gpu-rfork-canary
./build/spd-gdr/phos_gpu_rfork_canary --self-test
```

The binary is intentionally non-PIE and exports its symbols for the current
Mitosis/PhOS lab contract. Building or self-testing it does not load a kernel
module, start a daemon, transfer GPU data, or establish hardware evidence.
PhOS, Remoting, their configuration, and `libclient.so` are external lab
inputs and are not bundled here. The source process must be started from
`exec` with `GLIBC_TUNABLES=glibc.pthread.rseq=0`; setting it after process
startup does not satisfy the current 5.14 compatibility boundary.

For a hardware run, start one target-local PhOS/Remoting daemon per machine,
using disjoint PhOS UUID lanes. Start the source canary first and wait for its
`parent_prepared` event before starting the target launcher. The parent must
retain its Mitosis handler until the target emits a terminal
`canary_complete` event; only then may the orchestrator open the source
release gate. Each run uses fresh role-token paths, handler ID, run cookie,
clone ticket, daemon configuration, logs, and GDR endpoint.
