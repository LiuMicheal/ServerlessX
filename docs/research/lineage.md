# Research lineage

ServerlessX is a research integration effort, not a replacement claim for the
projects it studies. The doctoral-research architecture consists of Wiseswap as
the programmable RDMA substrate and three application-facing systems:
ServerlessPD, ServerlessRec, and ServerlessLSM.

The repository keeps the newly arranged SPD contract, TinyLlama workload, owned
native GDR source, and owned C remote-fork userspace boundary needed to make
that boundary reviewable. Historical experiment trees remain reference material
in their original controlled workspaces. No PhoenixOS, PhoenixOS-Remoting,
KRCore, Mitosis, Mooncake, kernel module, VM image, patch series, or generated
build product is copied here.

## Relationship of research systems

```text
Wiseswap: kernel-space programmable RDMA substrate
        |
        +-- RDMA Fork -----------------> ServerlessPD
        +-- RDMA Map ------------------> ServerlessRec
        +-- RDMA mmap + RDMA Fork -----> ServerlessLSM
```

- **Wiseswap** studies pre-created QP pools, virtual QPs, kernel-path
  scheduling, NIC-side work-request chains, and endpoint-selected multipath.
- **ServerlessPD** applies RDMA Fork to GPU-context cloning and disaggregated
  prefill/decode execution.
- **ServerlessRec** applies demand-paged RDMA Map to remote embedding memory for
  recommendation inference.
- **ServerlessLSM** applies RDMA mmap and independently scalable background
  functions to disaggregated LSM-tree storage.

Only ServerlessPD-centered implementation boundaries are included in this
snapshot: the portable SPD contract, TinyLlama workload, C++ native GDR source,
and C rfork userspace runtime. The other three systems are research lineage and
roadmap entries, not bundled implementations or runnable profiles.

The historical `serverlesspd.*.v1` identifiers and legacy PKV layout are kept in
the portable contract to avoid silently changing the experiment wire format.
They should not be presented as a generic public API until a versioned SPI and
converter are designed.

For exact included-code origins, see
[provenance/included-code.json](../../provenance/included-code.json). For
external URLs, revisions, license observations, and whether anything is bundled,
see [provenance/upstreams.json](../../provenance/upstreams.json).
