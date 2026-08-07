# ServerlessX

ServerlessX is a research codebase for composing three serverless systems:

- **SPD**: disaggregated prefill/decode execution and GPU-state transfer.
- **SRec**: recovery mechanisms for serverless execution.
- **SLSM**: lifecycle and state management for serverless workloads.

This private development snapshot makes the SPD contract inspectable and
runnable on an ordinary CPU host. It also includes the owned C/C++ native GDR
and remote-fork userspace boundaries plus hardware-free tests. It does **not**
yet claim that a fresh machine can run the GPU, RDMA, PhoenixOS, SRec, or SLSM
paths.

## Start here

You need Python 3.9 or newer. The bootstrap CPU path has no third-party Python
dependency and makes no privileged system change.

```bash
git clone https://github.com/LiuMicheal/ServerlessX.git
cd ServerlessX
./sx doctor
./sx plan --profile cpu
./sx run spd --profile cpu
./sx verify --latest
make test
```

The run writes a machine-readable result under `runs/`. The directory is local
evidence and is intentionally ignored by Git. See [QUICKSTART.md](QUICKSTART.md)
for the expected flow and troubleshooting steps.

## What works today

| Path | Included | Executable in this bootstrap | What it establishes |
| --- | --- | --- | --- |
| SPD CPU contract simulation | Yes | Yes | Metadata validation, ordered acknowledgements, ownership transitions, and fail-closed protocol behavior |
| TinyLlama CUDA/TCP workload | Yes | Not declared hardware-ready | Real prefill/decode implementation plus tests that use fakes and local sockets without CUDA hardware |
| Native RDMA DMA-BUF | C++ source and build | Build only | CUDA VMM/DMA-BUF/native RC implementation; no hardware success claim |
| Remote-fork userspace boundary | C source and tests | Tests only | Role token, ioctl, FD ownership, and fail-closed audit behavior; no kernel module |
| PhoenixOS lab backend | Reference only | No | Historical experiment context only; no PhoenixOS, Remoting, KRCore, or Mitosis code is bundled |
| SRec | Documentation placeholder | No | Planned ServerlessX subsystem boundary |
| SLSM | Documentation placeholder | No | Planned ServerlessX subsystem boundary |

The CPU path is a protocol simulation. It does not transfer a real KV cache,
load TinyLlama, exercise CUDA, measure network performance, prove GPU Direct
RDMA, or reproduce a PhoenixOS remote-fork experiment.

## Choose a path

- New user: read [the quickstart](QUICKSTART.md).
- GPU/RDMA operator: read [hardware profiles](docs/how-to/hardware-profiles.md)
  before running anything.
- Systems reader: start with [the architecture](docs/concepts/architecture.md)
  and the [native GDR boundary](native/spd-gdr/README.md).
- Research reader: start with [system lineage](docs/research/lineage.md),
  [included-code provenance](provenance/included-code.json), and
  [upstream provenance](provenance/upstreams.json).
- Contributor or coding agent: follow [AGENTS.md](AGENTS.md).
- Release reviewer: read [the licensing boundary](provenance/licensing.md).

## Repository map

```text
src/serverlessx/       Executable Python package
native/spd-gdr/        C++ CUDA DMA-BUF/RDMA data plane and probes
runtime/rfork/         C remote-fork userspace ABI, runtime, and tests
backends/              External Mitosis and PhOS identity records
systems/               Human-readable SPD, SRec, and SLSM boundaries
profiles/              Capability and deployment declarations
deploy/                Deployment entry-point documentation
tests/                  Standard-library unit and contract tests
testkit/                Result schemas and verification support
evidence/               Policy for publishable evidence
provenance/             Upstream identity and licensing observations
docs/                   Concepts, operations, references, and research notes
.agents/skills/         Repository-local workflow for coding agents
```

Profiles are declarations, not promises. `./sx doctor` observes the host,
`./sx plan` explains a proposed path, and only `./sx run` creates a run result.
The repository never installs a driver, loads a kernel module, changes a
network interface, starts a VM, or invokes `sudo` by itself.

## Using an AI coding agent

A user may give an agent this repository URL and ask it to prepare a run. The
repository provides both [operator rules](AGENTS.md) and a local deployment
skill so the agent can inspect the host, choose an executable compatible
profile, present a plan, and verify the result. Humans remain in control of
privileged actions: no agent is authorized by this repository to change the
kernel, drivers, RDMA configuration, networking, VMs, or shared services.

## Private bootstrap and licensing status

This repository currently has no project-wide `LICENSE`. Copyright ownership,
institutional rights, and third-party lineage are being audited before a public
release. Until a license is committed, do not assume permission to use, modify,
or redistribute this repository beyond access explicitly granted by its
rights holders.

The files under `native/spd-gdr/` retain individual Apache-2.0 SPDX identifiers.
The remaining repository, including the C rfork runtime, is still covered by
the project-wide ownership and licensing review. External projects are
identified for research provenance only. Their source,
patches, binaries, models, and build products are not bundled here. In
particular, PhoenixOS-Remoting and KRCore had no repository-root license at the
revisions observed during this audit; they must not be copied into a release
without separate rights clearance. See [provenance/licensing.md](provenance/licensing.md).
