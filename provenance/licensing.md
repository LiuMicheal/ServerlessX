# Licensing boundary

This private bootstrap deliberately has no top-level `LICENSE`. Copyright
ownership, institutional rights, contributor agreements, and third-party
redistribution terms are still under review. Until that review is complete,
access to this private repository must not be interpreted as a license to copy,
modify, publish, or redistribute it.

`included-code.json` records the observed Git origins and bootstrap changes for
the included Python, C, and C++ files. The C++ files under `native/spd-gdr/`
retain their existing Apache-2.0 SPDX identifiers. The C files under
`runtime/rfork/` had no file-level license marker at the audited source revision.
The final ownership and release license for the repository still need to be
confirmed with all relevant institutions and contributors. This document is a
process boundary, not a grant of rights.

## External projects

`provenance/upstreams.json` records URLs and exact revisions observed while
building the research work. It also records whether a license was observed and
whether any material is bundled. Every record currently says `bundled: false`.

In particular:

- PhoenixOS is recorded as Apache-2.0 at the audited upstream revision, but its
  local topic patches are not shipped here.
- Mitosis is recorded as MIT at the audited upstream revision, but its source,
  kernel pieces, and patches are not shipped here.
- PhoenixOS-Remoting and KRCore are recorded as `NOASSERTION` because a
  distributable license was not established for the observed revisions. They
  must remain reference-only unless rights are separately cleared.
- Mooncake is an optional historical reference and is not needed by the CPU
  path or bundled in this repository.

A paper, a public GitHub page, or a citation gives useful attribution but does
not by itself grant source redistribution rights. Do not vendor an external
tree, copy its patch context, publish its binaries, or automate an unlicensed
download from this repository.

## Release gate

Before a public release, record the copyright holder(s), institutional review,
contributor acknowledgements, dependency licenses, notices, and a chosen
project license in a reviewed change. Before ASF incubation, run a separate IP
clearance and dependency review; do not imply ASF affiliation before acceptance.
