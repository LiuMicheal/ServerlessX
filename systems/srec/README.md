# ServerlessRec (SRec)

ServerlessRec is the memory-sensitive recommendation-inference system in the
ServerlessX research architecture. Its RDMA Map primitive maps remote embedding
table shards into short-lived instances and fetches data on demand without
requiring a resource-node CPU on the lookup path.

This development snapshot contains a research status boundary only. It does not
include a ServerlessRec implementation, an RDMA Map runtime, a runnable profile,
or evidence that reproduces the paper's performance claims.

Future source must define the embedding-table mapping contract, demand-paging
semantics, ownership and reclamation rules, failure behavior, provenance, and
verification evidence before the subsystem is advertised as executable.
