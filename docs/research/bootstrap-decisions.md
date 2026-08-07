# Bootstrap decision record

This note records the repository decisions made before the first private push.
It is not a claim that every historical experiment has been published.

## Project shape

- Use the umbrella name **ServerlessX** for SPD, SRec, and SLSM.
- Make the repository understandable to a person before optimizing it for an
  automated agent.
- Also provide machine-readable profiles, JSON command output, `AGENTS.md`, and
  a repository-local skill so an agent can help an operator safely.
- Keep the canonical checkout and future migration work on the newer primary
  GPU environment rather than treating the older experiment host as the
  project home.

## First runnable boundary

- Publish an executable CPU SPD contract simulation with no third-party runtime
  dependency.
- Include the TinyLlama CUDA/TCP workload and its hardware-free tests so readers
  can inspect substantive implementation code.
- Describe native RDMA DMA-BUF and PhOS as unavailable profiles until their
  external inputs and deployment gates are independently reviewable.
- Reserve clear subsystem boundaries for SRec and SLSM without inventing code
  or readiness claims.

## IP and dependency boundary

- Do not bundle PhoenixOS, PhoenixOS-Remoting, KRCore, Mitosis, Mooncake, their
  patches, generated source, binaries, kernel modules, containers, or VM images
  in the bootstrap.
- Record exact upstream URLs and revisions for attribution and audit.
- Treat a public paper or repository as research visibility, not automatic
  permission to redistribute source.
- Keep dependencies without an established license, especially the observed
  Remoting and KRCore revisions, reference-only.
- Do not add a project-wide license until contributor and institutional rights
  are reviewed.

## Public and ASF direction

- The conference announcement can point to a clean, honest repository whose CPU
  path works and whose stronger paths are visibly bounded.
- A later public release needs ownership review, license selection, contributor
  policies, sanitized evidence, and reproducible inputs.
- A later ASF proposal additionally needs clean IP history, compatible
  dependencies, community governance, and participation beyond one research
  group. The bootstrap must not imply ASF affiliation.

## Open decisions

- Final project license and copyright holder(s).
- Which collaborators will be acknowledged as authors, contributors, mentors,
  or downstream users, with their consent.
- A distributable CUDA/TCP environment and model acquisition procedure.
- A native RDMA DMA-BUF public backend that does not require unlicensed code.
- Whether the historical PhOS integration can ever become independently
  distributable or should remain an external lab adapter.
- Versioned public interfaces between SPD, SRec, and SLSM.
