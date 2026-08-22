# DLRM remote embedding table experiment

This directory is a small application-level proof of concept for ServerlessRec.
It imports the unmodified `DLRM_Net` class from the official
`facebookresearch/dlrm` checkout and replaces one CPU `EmbeddingBag` with a
zero-copy view over a DMerge RC-only demand-paged mapping.

The experiment is intentionally narrow:

- three tables, one remote table;
- 16,384 rows x 16 `float32` values (1 MiB, 256 pages);
- CPU-only inference, no training, GPU, TorchRec, or dataloader;
- a deterministic table and deterministic model inputs for exact comparison;
- the C producer is only a fixture for the existing lab syscall ABI. It does
  not bundle or replace DMerge or its kernel module.

## Files

- `dmerge_embedding_producer.c`: MN-side table publisher.
- `dlrm_remote_embedding.py`: CN-side DLRM adapter, ioctl setup, lookup, and
  metric output.

The official DLRM source is intentionally not copied into this repository. The
measured run used the checkout at `/home/liumx/Code/dlrm-facebookresearch`,
commit `9bc1bc3602aea22afad3206910cec50f2dbe7364`.

## Reproduce in the lab VMs

The command sequence assumes both SRec guests already have the RC-only
`heap.ko`, `/dev/mitosis-syscalls`, and their second-RNIC VF configured. It does
not change Host or VF configuration.

Build the producer in the MN guest:

```bash
cc -O2 -Wall -Wextra -Werror -std=c11 dmerge_embedding_producer.c \
  -o dmerge_embedding_producer
./dmerge_embedding_producer producer 16384 16 0
```

In an isolated Python environment on the CN guest, install CPU-only PyTorch and
NumPy, then run:

```bash
python dlrm_remote_embedding.py \
  --mode remote \
  --official-root /path/to/dlrm-facebookresearch \
  --rows 16384 --dim 16 --table-id 0 --batch-size 8 --warm-iters 20 \
  --gid '<MN RoCEv2 GID>' --machine-id 1 --heap-hint '<Register return value>'
```

`--heap-hint` is the value returned by the MN `Register` ioctl; it is not a
stable constant. Stop the producer after the CN process exits. Do not unload
`heap.ko` while RDMA CM work may still be queued.

The script prints one `RESULT_JSON=...` record. It reports setup latency,
cold/warm forward latency, process fault deltas, expected unique pages, output
checksums, and absolute output error against an identical local-table model.
