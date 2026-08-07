# Native RDMA DMA-BUF profile

**ID:** `rdma_dmabuf`
**Maturity:** source preview
**Executable:** build only; no deployment workflow

This profile includes the native GPU Direct RDMA source path: a CUDA VMM
allocation exported through DMA-BUF and read by an RDMA peer without a staging
slab. `make spd-gdr` compiles the library and probes when the required SDKs are
present. Compilation is not a driver installer or a claim that the path works
on a fresh host.

An executable deployment version must verify kernel and NVIDIA driver support, an
authorized RDMA device and gid, exact ABI/build inputs, isolation, cleanup, and
an independently hashed result. It must ask before any privileged operation.
