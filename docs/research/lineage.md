# Research lineage

ServerlessX is a research integration effort, not a replacement claim for the
projects it studies. The current private work grew from experiments around
serverless prefill/decode execution, GPU state movement, remote fork/recovery,
and lifecycle management.

The repository keeps the newly arranged SPD contract, TinyLlama workload, owned
native GDR source, and owned C remote-fork userspace boundary needed to make
that boundary reviewable. Historical experiment trees remain reference material
in their original controlled workspaces. No PhoenixOS, PhoenixOS-Remoting,
KRCore, Mitosis, Mooncake, kernel module, VM image, patch series, or generated
build product is copied here.

## Relationship of components

```text
ServerlessX orchestration
        |
   SPD contract ---- TinyLlama P/D workload
        |                    |
   C rfork ABI         C++ native GDR
        |
  future SRec / SLSM interfaces
        |
  separately supplied, audited backends (CUDA/TCP, RDMA, PhOS lab)
```

The historical `serverlesspd.*.v1` identifiers and legacy PKV layout are kept in
the portable contract to avoid silently changing the experiment wire format.
They should not be presented as a generic public API until a versioned SPI and
converter are designed.

For exact included-code origins, see
[provenance/included-code.json](../../provenance/included-code.json). For
external URLs, revisions, license observations, and whether anything is bundled,
see [provenance/upstreams.json](../../provenance/upstreams.json).
