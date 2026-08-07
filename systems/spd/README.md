# SPD

SPD is the first implemented ServerlessX subsystem. The repository exposes four
layers:

1. A portable contract simulator that validates fixed experiment metadata and
   an ordered acknowledgement/ownership ledger on the CPU.
2. A TinyLlama prefill/decode workload implementation whose runtime interfaces
   can use CUDA and TCP host staging when a separately validated environment is
   supplied.
3. An owned C++ native GDR library and probes for CUDA VMM, DMA-BUF,
   `ibv_reg_dmabuf_mr`, and RC transfer.
4. An owned C userspace remote-fork boundary with injected-I/O tests; the Rust
   Mitosis kernel module remains an external backend.

The source package intentionally owns no kernel, VM, CUDA driver, RDMA device,
or cluster lifecycle. A deployment backend must establish those resources and
must provide evidence for each claim it makes.

The contract keeps the historical `serverlesspd.*.v1` wire identifiers and the
legacy PKV tensor layout. It is a fixed TinyLlama CCF-A experiment contract:
22 layers, four KV heads, head dimension 64, prompt length 128, and eight
output tokens. It is not yet a model-agnostic public SPI.

See `src/serverlessx/spd/`, `native/spd-gdr/`, `runtime/rfork/`, and
`tests/spd/` for the included boundaries.
