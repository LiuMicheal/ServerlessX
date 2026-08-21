# ServerlessRec (SRec)

ServerlessRec is the memory-sensitive recommendation-inference system in the
ServerlessX research architecture. Its RDMA Map primitive maps remote embedding
table shards into short-lived instances and fetches data on demand without
requiring a resource-node CPU on the lookup path.

This development snapshot still contains no ServerlessRec implementation, RDMA
Map runtime, or runnable profile. It now records one narrow external RC-only
transport smoke in [the lab evidence note](../../docs/experiments/srec-dmerge-rc-smoke-20260821.md).
That note is not a ServerlessX run and does not reproduce the paper's
performance claims.

Future source must define the embedding-table mapping contract, demand-paging
semantics, ownership and reclamation rules, failure behavior, provenance, and
verification evidence before the subsystem is advertised as executable.
