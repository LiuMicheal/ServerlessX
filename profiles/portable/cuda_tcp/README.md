# CUDA/TCP profile

**ID:** `cuda_tcp`
**Maturity:** source and test boundary
**Executable:** no in this bootstrap

The repository includes a TinyLlama prefill/decode implementation with a
length-prefixed TCP host-staging transport. Its tests use fake model runtimes
and local sockets so they can run without CUDA or a checkpoint. The source is
included for review and future integration; no hardware deployment is claimed
by the first profile declaration.

A future executable declaration must identify a compatible CUDA/PyTorch/
Transformers environment, model and tokenizer digests, process lifecycle,
transport limits, and reproducible evidence. It must not silently fall back to
the CPU simulation and call that a CUDA run.
