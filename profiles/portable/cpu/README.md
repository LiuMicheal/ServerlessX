# Portable CPU profile

**ID:** `cpu`
**Maturity:** executable (portable tier)
**Executable:** yes

This is the default profile for a new checkout. It runs the SPD contract
simulation with the standard library and writes a small JSON result under
`runs/`. It does not allocate CUDA memory, open RDMA devices, transfer a real
KV cache, load a model, or access a VM.

```bash
./sx doctor --format json
./sx plan --profile cpu --format json
./sx run spd --profile cpu --format json
./sx verify --latest --format json
```

The machine-readable declaration is adjacent to this file. If it says the
profile is unavailable, stop and inspect the plan instead of overriding it.
