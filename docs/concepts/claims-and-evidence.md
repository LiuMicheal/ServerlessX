# Claims and evidence

ServerlessX uses four evidence levels:

1. **Simulation**: deterministic local logic, such as the CPU SPD ledger.
2. **Hardware-free test**: a real implementation exercised with fake runtime
   objects or local sockets, such as the TinyLlama workload tests.
3. **Bounded hardware run**: a run on named hardware with an inventory,
   hashes, lifecycle checks, and a result verifier.
4. **Reproducible deployment**: a bounded run that an independent operator can
   repeat from documented, lawfully redistributable inputs.

The first bootstrap contains levels 1 and 2 only. It may contain links to level
3 research records, but those records are not automatically public evidence and
do not turn the corresponding profile executable.

## Minimum evidence shape

A result should identify its run ID, profile, source revision, host facts or
fixture mode, terminal status, and claim boundary. Hashes should be recorded
for material inputs rather than copied into the repository. A failed or partial
run must remain visibly failed.

Do not report latency, throughput, memory savings, model quality, zero-copy,
or end-to-end migration unless the result contains the measurement and the
specific setup required to interpret it.
