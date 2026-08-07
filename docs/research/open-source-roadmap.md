# Open-source roadmap

The repository is intentionally staged so that opening the GitHub URL gives a
reader something honest to run immediately while preserving a path toward a
larger public project.

## Bootstrap (this repository)

- CPU SPD contract simulation and standard-library tests.
- TinyLlama CUDA/TCP implementation source with hardware-free tests.
- C++ native DMA-BUF GDR source and build rules.
- C remote-fork userspace boundary with injected-I/O tests.
- External backend records for Mitosis and PhOS.
- Profile declarations for CUDA, RDMA DMA-BUF, and PhOS deployment work.
- Provenance and licensing records with external source excluded.

## Public release gate

Before making this repository public, the owners should complete contributor and
institutional rights review, choose a project license, add contribution and
security policies, sanitize evidence and paths, and decide which external
backends can be distributed under compatible licenses. A link to a paper or an
upstream GitHub repository is not a substitute for those permissions.

## ASF exploration gate

An ASF incubation proposal would require a clean IP history, a clear list of
authors and grants, compatible dependencies, reproducible build inputs,
community governance, and a contributor workflow that is independent of one
research lab. The current bootstrap is a technical and documentation starting
point, not an ASF-ready release.

Do not add Apache headers or claim ASF affiliation until the relevant legal and
community process has actually approved them.
