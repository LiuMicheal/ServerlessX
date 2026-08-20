# Architecture

ServerlessX is a multi-language systems project. Languages follow ownership and
runtime boundaries; they are not selected to influence repository statistics.

## Research system map

The doctoral-research architecture connects one RDMA substrate to three
application-facing systems through distinct remote-resource primitives:

```text
Wiseswap: programmable RDMA network substrate
        |
        +-- RDMA Fork -----------------> ServerlessPD
        +-- RDMA Map ------------------> ServerlessRec
        +-- RDMA mmap + RDMA Fork -----> ServerlessLSM
```

This map describes the research lineage, not the contents of the current
development snapshot. Only the ServerlessPD-centered boundaries described below
have implementation code in this repository. Wiseswap, ServerlessRec, and
ServerlessLSM have no bundled runnable implementation or profile. SLSM has a
separate external Stage 1 correctness note for an RC SST-fetch mechanism; that
note does not represent RDMA mmap, LSM semantics, or repository code.

## Current repository implementation

```text
Human / coding agent
        |
        v
Python control and evidence plane
  sx, profiles, contracts, orchestration, verification
        |
        +-------------------------------+
        |                               |
        v                               v
C++ SPD GDR data plane             C rfork userspace runtime
CUDA VMM / DMA-BUF / verbs         role / ioctl / FD ownership
        |                               |
        v                               v
CUDA + RDMA host stack             external kernel backend
                                    Mitosis fork.ko (Rust)
        |
        v
optional external PhOS backend (C++)
```

## Owned boundaries

- `src/serverlessx/`: Python host discovery, planning, protocol simulation,
  workload code, evidence, and verification.
- `native/spd-gdr/`: C++ direct GPU-memory data plane. The default build uses
  native RC and excludes the optional Mooncake implementation.
- `runtime/rfork/`: C userspace ABI and fail-closed ownership logic around a
  Mitosis-compatible device.

## External boundaries

- Mitosis provides the Rust remote-fork kernel backend and produces `fork.ko`.
  Its owned integration is a patch set, not an independent ServerlessX kernel.
- PhoenixOS provides the C++ checkpoint/restore substrate used by the historical
  PhOS experiment path.
- KRCore and PhoenixOS-Remoting remain excluded because no repository-root
  license was observed at the pinned revisions.

Backend records live under `backends/`. A backend being documented does not
mean it is bundled, runnable, or acceptable for an ASF release.

## Maturity boundaries

The CPU contract and rfork unit test run without hardware. Native GDR source can
be compiled on a prepared CUDA/RDMA host. Actual GPU transfer, remote fork, and
PhOS reproduction remain separate hardware evidence levels and must not be
inferred from a successful unit test or build. The external SLSM Stage 1 Guest
run is likewise a narrow correctness result, not a bundled deployment,
performance result, or end-to-end LSM claim.

See [current status](../status.md) for the exact included, build-only,
reference-only, and roadmap boundaries.
