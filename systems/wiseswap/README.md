# Wiseswap

Wiseswap is the programmable RDMA network substrate in the ServerlessX research
architecture. The research design moves short-lived serverless instances off
the RDMA connection-setup critical path through pre-created physical QP pools
and lightweight virtual QPs. It also studies kernel-path scheduling, NIC-side
work-request chains, and endpoint-selected multipath behavior.

This development snapshot contains a research status boundary only. It does not
include a Wiseswap implementation, kernel module, runnable profile, or evidence
that reproduces the paper's performance claims. The included SPD native GDR
library and rfork userspace runtime must not be described as a bundled Wiseswap
implementation.

Future source must have independently reviewable ownership, provenance,
dependency licenses, build inputs, tests, and hardware evidence before Wiseswap
is advertised as executable from this repository.
