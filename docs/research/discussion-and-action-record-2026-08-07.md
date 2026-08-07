# Discussion and action record through 2026-08-07

This document records the decisions that led to the first two ServerlessX
repository revisions. It is written to remain safe if the repository becomes
public. Personal contact lists, private infrastructure addresses, credentials,
and host-specific access instructions belong in a separate private note.

## Project objective

ServerlessX is the umbrella project for three doctoral-research systems:

- SPD: disaggregated prefill/decode execution, remote fork, and GPU-state
  movement;
- SRec: recovery mechanisms for serverless execution;
- SLSM: lifecycle and state management for serverless workloads.

The repository must serve two audiences at once. A person should understand the
system, maturity, and claim boundaries from the first screen. A coding agent
should be able to inspect the host, select a supported profile, present a plan,
execute only approved operations, and return verifiable evidence.

The long-term objective is a useful open-source system, not only a paper
artifact. A user with the required hardware should eventually be able to give
the repository URL to an agent and receive help deploying the strongest
compatible backend without granting implicit permission for host-wide changes.

## Repository and operational decisions

- Use the umbrella repository name `ServerlessX`.
- Keep the canonical working checkout on the newer primary GPU host rather than
  the older experiment host.
- Begin as a private repository while ownership and dependency rights are
  audited.
- Use a repository-scoped GitHub Deploy Key rather than a personal SSH key.
- Keep human-readable output as the default and provide deterministic JSON for
  automation.
- Separate observation, planning, execution, and verification:
  `doctor -> plan -> run -> verify`.
- Require explicit approval before package installation, driver/kernel changes,
  network changes, VM lifecycle operations, or cleanup of unowned resources.

The first push created `main` at commit `7fc1761` with 51 files. It was produced
and pushed by Git on the primary GPU host. Python 3.9 validation passed 34 unit
tests, the CPU contract run verified by exact run ID, the remote `main` ref
matched the local commit, and the worktree was clean after push.

## First bootstrap boundary

The initial repository intentionally contained only material that could be
reviewed quickly without bundling external system trees:

- a Python CLI and machine-readable profiles;
- an executable CPU simulation of the SPD metadata, ACK, epoch, replay, and
  ownership contract;
- the TinyLlama P/D workload source and hardware-free tests;
- documentation, provenance, release gates, and agent instructions;
- unavailable declarations for CUDA/TCP, native RDMA, and the PhOS lab path.

The CPU profile proves protocol behavior only. It does not transfer a real KV
cache, execute a model, use CUDA, establish RDMA, load a kernel module, or
reproduce a PhOS experiment.

This boundary was legally conservative and operationally honest, but it was too
narrow to represent the system architecture at a conference. The pushed code
had 3,597 lines of Python and no directly tracked C, C++, CUDA, Rust, or Go.
A visitor could reasonably classify it as an artifact harness rather than a
systems implementation.

## Native-core correction

The existing ServerlessPD research tree contains substantial native work:

- C++/CUDA native RC, CUDA VMM, DMA-BUF, and optional Mooncake GDR paths;
- a C userspace boundary for remote-fork role, ioctl, FD ownership, and audit
  state;
- CUDA workloads and hardware probes;
- Python orchestration and evidence validation;
- integration changes maintained against Mitosis and PhoenixOS.

The second repository revision therefore adds the highest-confidence owned
native boundary instead of adding language placeholders:

- `native/spd-gdr/`: Apache-2.0-marked C++ direct DMA-BUF/RDMA library and
  probes;
- `runtime/rfork/`: C11 userspace ABI/runtime and hardware-free unit test;
- `backends/`: machine-readable records for external Mitosis and PhOS systems;
- root Make targets for portable Python/C tests and optional GDR compilation.

Python remains the control and evidence plane. C/C++ is the owned native data
and runtime boundary. No Go code is added because the Go files observed in the
historical PhOS tree are unchanged upstream build scripts, not ServerlessX
core. No Rust source is added merely to change GitHub's language chart.

## Upstream and copyright analysis

The research implementation was developed around public academic systems. A
paper, citation, or public GitHub page does not itself grant permission to copy,
modify, or redistribute source. Each code path must be evaluated using the
license present at the exact revision.

Observed boundaries:

- PhoenixOS: Apache-2.0 at the audited base revision. A properly attributed fork
  or patch series can be distributed under its license, but it remains an
  upstream-derived backend rather than independently owned ServerlessX core.
- Project Mitosis: MIT at the audited base revision. ServerlessPD integration
  commits can be published with the upstream notice and accurate authorship.
- PhoenixOS-Remoting: no repository-root license observed at the audited
  revision. Source, binaries, and context-bearing patches remain excluded.
- KRCore: no repository-root license observed at the audited revision. It
  remains a blocker for distributing a complete Mitosis build as an ASF-ready
  core.
- Mooncake: Apache-2.0 optional transport. The owned direct GDR build compiles
  with `SPD_GDR_DIRECT_ONLY` and does not require Mooncake.

Links to upstream repositories are useful attribution and discovery, but they
do not turn an unresolved dependency into a distributable component. Automatic
downloads are also avoided for dependencies whose release rights are unclear.

## Rust kernel and PhOS positioning

Historical remote fork uses a Rust `fork.ko` built from Project Mitosis. Most of
that Rust implementation is upstream work; ServerlessPD contributes a bounded
pure-RC patch set. It must be described as the Mitosis backend used by
ServerlessX, not as ServerlessX-owned Rust kernel core.

Mitosis itself is MIT-licensed, so contact with its authors is not a legal
prerequisite for a correctly attributed fork. The unresolved KRCore dependency
still prevents a clean portable or ASF distribution. Long-term alternatives
are to replace that dependency, implement an independently owned kernel
backend, or keep remote fork outside the release boundary.

The PhOS branch contains substantial ServerlessPD integration work on an
Apache-2.0 PhoenixOS base, especially C++ template serialization, clone attach,
CUDA VMM restore, and GDR adapters. Those changes may become a separately
attributed backend patch series. The unlicensed Remoting dependency prevents
presenting the complete historical path as the portable public core.

## Reproducibility design

The repository should converge on this user workflow:

1. `./sx doctor --format json` performs read-only host discovery.
2. `./sx plan --profile auto --format json` selects only an available,
   compatible profile.
3. The user reviews every host-wide or privileged operation.
4. `./sx run ...` executes a bounded workflow and creates a unique run ID.
5. `./sx verify --run-id ...` validates that exact result, never an unrelated
   older run.

Source availability, buildability, hardware execution, and a publishable
performance claim are separate maturity levels. Compiling the GDR library does
not prove that a host supports CUDA DMA-BUF or that an RDMA transfer succeeded.

## Conference communication

Until all backends are independently deployable, the accurate announcement is:

> ServerlessX has opened its control/evidence plane, SPD native GDR data plane,
> checked remote-fork userspace boundary, and reproducible workload tests.
> Mitosis and PhOS remain separately attributed research backends.

It is inaccurate to announce that the complete historical ServerlessX stack is
portable or independently reproducible. Slides should distinguish code that is
included, source that is available but not runnable, external backends, and
claims supported by current evidence.

## ASF direction

An ASF incubation attempt needs more than an Apache-style directory or license:

- clear ownership and institutional approval for every contributed file;
- accurate author history and a Software Grant/IP clearance path;
- compatible dependencies with complete license and notice records;
- a release that provides meaningful functionality without unresolved
  Remoting or KRCore dependencies;
- public contribution, review, security, and governance processes;
- participation and decision-making beyond a single research group;
- no implication of ASF affiliation before acceptance.

Existing patch files with placeholder authors or incomplete mail headers must
be regenerated from the real commits before publication. Rewriting a patch's
author to the project name damages provenance and is not acceptable for ASF IP
review.

## Community and collaboration

The project owner has access to multiple university research groups, faculty,
doctoral students, master's students, alumni, and industry collaborators. That
is a strong seed network, but employment, supervision, or laboratory membership
does not automatically create an open-source contributor community.

Before naming people publicly, obtain consent and decide whether each person is
an author, code contributor, reviewer, mentor, user, committer candidate, or
institutional stakeholder. Governance roles should follow sustained public
work rather than organizational hierarchy.

## Open decisions

- Confirm copyright holders and institutional rights for the Python bootstrap
  and the C rfork runtime.
- Choose and approve the project-wide release license; no root `LICENSE` is
  committed yet.
- Decide whether the native GDR file-level Apache-2.0 grant is sufficient for
  the first public preview or requires additional institutional confirmation.
- Regenerate publishable Mitosis and PhoenixOS patch series with real authors.
- Replace or remove KRCore and Remoting from any portable release dependency
  graph.
- Import SRec and SLSM only when their code, tests, ownership, and interfaces
  are concrete; documentation placeholders must not imply implementation.
- Build contributor, security, conduct, release, and governance policies before
  an ASF proposal.
