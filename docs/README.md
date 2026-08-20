# Documentation guide

Use the documents in this order:

1. [Getting started](getting-started/README.md) for the command lifecycle.
2. [Current status](status.md) for included, executable, reference-only, and
   roadmap boundaries.
3. [Architecture](concepts/architecture.md) for the research system map plus
   language and ownership layers.
4. [Profiles](concepts/profiles.md) for capability and maturity semantics.
5. [Hardware profiles](how-to/hardware-profiles.md) before considering CUDA,
   RDMA, or the PhOS lab.
6. [CLI reference](reference/cli.md) for stable command shapes.
7. [Research lineage](research/lineage.md) and
   [open-source roadmap](research/open-source-roadmap.md) for context.
8. [Discussion and action record](research/discussion-and-action-record-2026-08-07.md)
   for the decisions and work through the native-core import.
9. [SPD 5.14 P0-P2 progress](research/spd-5.14-p0-p2-progress.md) for the
   sanitized compile gate and research boundary.
10. [SLSM Stage 1 RC SST-fetch note](research/slsm-stage1-rc-poc-20260820.md)
    for the external two-Guest correctness evidence and its claim boundary.

Documentation describes the boundary that the code and profile declarations
actually support. When evidence and prose disagree, the narrower claim wins
until the evidence or declaration is corrected.
