# Current status

This page is the concise source of truth for release-scope statements in talks,
papers, demos, and repository descriptions. It was last reviewed on 2026-08-08.
When prose and executable profiles disagree, the narrower profile claim wins.

## Access and licensing

- The GitHub repository is currently a private development snapshot.
- There is no project-wide `LICENSE` yet. Access is not permission to copy,
  modify, publish, or redistribute the repository.
- Copyright ownership, institutional rights, contributor terms, and third-party
  lineage remain under review.
- File-level Apache-2.0 identifiers under `native/spd-gdr/` do not license the
  rest of the repository.

The project must not be described as an open-source release until the repository
is public and an approved project-wide license is committed. If the repository
becomes public before that review is complete, describe it only as a publicly
visible source preview and retain the licensing warning.

## Capability matrix

| Boundary | Source included | Current evidence | Accurate claim |
| --- | --- | --- | --- |
| ServerlessPD CPU contract | Yes | Executable CPU run and unit tests | Protocol and ownership simulation only |
| TinyLlama CUDA/TCP workload | Yes | Hardware-free tests | Inspectable source; not declared hardware-ready |
| SPD native GDR | C++ source and build rules | Build and probes compile on a prepared host | Buildable boundary; no repository-packaged transfer result |
| rfork userspace runtime | C source and tests | Injected-I/O unit tests | Checked userspace ABI; no bundled kernel module |
| Mitosis and PhOS backends | No; identity records only | Historical controlled-lab context | External, separately supplied research backends |
| Wiseswap | Status page only | No implementation or profile | Research lineage and roadmap only |
| ServerlessRec | Status page only | No implementation or profile | RDMA Map research lineage and roadmap only |
| ServerlessLSM | Status page only | No implementation or profile | RDMA mmap research lineage and roadmap only |

The performance numbers reported in the Wiseswap, ServerlessPD, ServerlessRec,
and ServerlessLSM papers are research results. They are not automatically
reproduced by the current GitHub quickstart.

## What a new user can run

On Python 3.9 or newer, without a GPU or RDMA device:

```bash
./sx doctor
./sx plan --profile cpu
./sx run spd --profile cpu
./sx verify --latest
make test
```

The commands produce protocol evidence, not CUDA, RDMA, latency, throughput, or
model-quality evidence. Hardware paths have separate requirements and maturity
gates documented under `profiles/` and
[hardware profiles](how-to/hardware-profiles.md).

## Participation

Current contribution priorities and validation requirements are documented in
[CONTRIBUTING.md](../CONTRIBUTING.md). No contributor, committer, mentor, ASF
affiliation, or institutional endorsement should be inferred unless it is
explicitly recorded with the relevant party's consent.
