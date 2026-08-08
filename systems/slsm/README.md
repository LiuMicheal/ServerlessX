# ServerlessLSM (SLSM)

ServerlessLSM is the I/O-intensive storage system in the ServerlessX research
architecture. It applies RDMA mmap to disaggregated LSM-tree storage, separates
foreground access from independently scalable flush and compaction functions,
and coordinates metadata without a centralized lock manager.

This development snapshot contains a research status boundary only. It does not
include a ServerlessLSM implementation, an RDMA mmap runtime, a runnable profile,
or evidence that reproduces the paper's performance claims.

Future source must define the mapping and metadata contracts, storage ownership,
failure and recovery behavior, provenance, and verification evidence before the
subsystem is advertised as executable.
