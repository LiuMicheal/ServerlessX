# ServerlessRec (SRec)

ServerlessRec is the memory-sensitive recommendation-inference system in the
ServerlessX research architecture. Its RDMA Map primitive maps remote embedding
table shards into short-lived instances and fetches data on demand without
requiring a resource-node CPU on the lookup path.

This development snapshot does not declare a general ServerlessRec runtime or
hardware profile. It contains two deliberately narrow lab artifacts: an
external RC-only transport smoke in [the lab evidence note](../../docs/experiments/srec-dmerge-rc-smoke-20260821.md),
and a small [DLRM remote embedding experiment](experiments/dlrm_remote_embedding/)
that consumes one table through the existing DMerge demand-paged mapping. The
measured result is recorded in [the DLRM evidence note](../../docs/experiments/srec-dlrm-remote-embedding-20260822.md).
Neither artifact reproduces the paper's performance claims or constitutes a
production RDMA Map runtime.

Future source must define the embedding-table mapping contract, demand-paging
semantics, ownership and reclamation rules, failure behavior, provenance, and
verification evidence before the subsystem is advertised as executable.
