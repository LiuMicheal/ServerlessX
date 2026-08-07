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
