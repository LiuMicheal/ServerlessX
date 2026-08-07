# Research lineage

ServerlessX is a research integration effort, not a replacement claim for the
projects it studies. The current private work grew from experiments around
serverless prefill/decode execution, GPU state movement, remote fork/recovery,
and lifecycle management.

The bootstrap keeps only the newly arranged SPD contract and TinyLlama workload
source needed to make that boundary reviewable. Historical experiment trees
remain reference material in their original controlled workspaces. No
PhoenixOS, PhoenixOS-Remoting, KRCore, Mitosis, Mooncake, kernel module, DSO,
VM image, patch series, or generated build product is copied here.

## Relationship of components

```text
ServerlessX orchestration
        |
   SPD contract ---- TinyLlama P/D workload
        |
  future SRec / SLSM interfaces
        |
  separately supplied, audited backends (CUDA/TCP, RDMA, PhOS lab)
```

The historical `serverlesspd.*.v1` identifiers and legacy PKV layout are kept in
the portable contract to avoid silently changing the experiment wire format.
They should not be presented as a generic public API until a versioned SPI and
converter are designed.

For exact upstream URLs, revisions, license observations, and whether anything
is bundled, see [provenance/upstreams.json](../../provenance/upstreams.json).
