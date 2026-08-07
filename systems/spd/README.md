# SPD

SPD is the first implemented ServerlessX subsystem. The bootstrap exposes two
layers:

1. A portable contract simulator that validates fixed experiment metadata and
   an ordered acknowledgement/ownership ledger on the CPU.
2. A TinyLlama prefill/decode workload implementation whose runtime interfaces
   can use CUDA and TCP host staging when a separately validated environment is
   supplied.

The source package intentionally owns no kernel, VM, CUDA driver, RDMA device,
or cluster lifecycle. A deployment backend must establish those resources and
must provide evidence for each claim it makes.

The contract keeps the historical `serverlesspd.*.v1` wire identifiers and the
legacy PKV tensor layout. It is a fixed TinyLlama CCF-A experiment contract:
22 layers, four KV heads, head dimension 64, prompt length 128, and eight
output tokens. It is not yet a model-agnostic public SPI.

See `src/serverlessx/spd/contract.py`,
`src/serverlessx/spd/tinyllama_pd.py`, and
`tests/spd/` for the executable boundary.
