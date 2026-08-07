# Native RDMA DMA-BUF profile

**ID:** `rdma_dmabuf`
**Maturity:** research plan
**Executable:** no in this bootstrap

This profile records the intended native GPU Direct RDMA path: a CUDA VMM
allocation exported through DMA-BUF and read by an RDMA peer without a staging
slab. The declaration is a requirements checklist, not a driver installer or a
claim that the path is available on a fresh host.

An executable version must verify kernel and NVIDIA driver support, an
authorized RDMA device and gid, exact ABI/build inputs, isolation, cleanup, and
an independently hashed result. It must ask before any privileged operation.
