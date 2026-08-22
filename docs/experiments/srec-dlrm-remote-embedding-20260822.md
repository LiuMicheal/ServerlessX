# SRec DLRM remote embedding experiment

Date: 2026-08-22

Status: **PASS (minimal hardware-backed functional run)**

This is the first application-level run of the ServerlessRec idea on the
existing SRec DMerge RC-only mapping. It is a functional PoC, not a production
embedding service and not a throughput claim for an optimized SRec runtime.

## Scope

The official DLRM model was used in its CPU path. One of three embedding tables
was replaced by `RemoteEmbeddingBag`, a small adapter with the same call shape
as PyTorch `EmbeddingBag`. The adapter points a NumPy/Torch view at the fixed
address installed by DMerge `Pull`; ordinary table indexing then triggers the
existing demand-page path.

The experiment deliberately excludes training, GPUs, TorchRec, DC/DCT, and any
change to SPD/SLSM. The DMerge kernel module and the C fixture remain external
lab artifacts; this repository contains only the adapter and reproducibility
code.

## Configuration

| item | value |
| --- | --- |
| official DLRM commit | `9bc1bc3602aea22afad3206910cec50f2dbe7364` |
| guest OS/kernel | Rocky Linux 9.8 / `5.14.0-687.10.1.el9_8.0.1.x86_64` |
| MN/CN | `srec-mn` / `srec-cn-mem02` |
| transport | second-RNIC VF, RoCEv2 GID index 3, DMerge RC-only |
| model tables | 3 |
| remote table | table 0, 16,384 rows x 16 `float32` |
| remote table size | 1,048,576 bytes, 256 x 4 KiB pages |
| batch size | 8, one index per table and sample |
| expected unique remote pages | 8 |
| runtime | isolated Python 3.9 venv, PyTorch `2.4.1+cpu`, NumPy `1.26.4` |
| seed | `20260822` |

The MN producer registered the table with checksum `296503.606547475` and
returned `heap_hint=1` for this clean run. The hint must be read from the
actual `Register` return value on every run.

## Measured metrics

All times are from `time.perf_counter_ns()` and are reported in microseconds.
The local baseline was run in a separate fresh CN process with the same model,
inputs, and venv. The remote run used a fresh process after both VM-side DMerge
services were reinitialized.

| metric | local table | remote table |
| --- | ---: | ---: |
| ConnectSession | n/a | 81,072.009 us |
| Pull (`eager_fetch=false`) | n/a | 120.554 us |
| cold forward | 864.349 us | 6,772.417 us |
| warm forward p50 | 122.214 us | 665.826 us |
| warm forward p95 | 140.232 us | 784.356 us |
| warm throughput (1 / p50) | 8,182.4 forward/s | 1,501.9 forward/s |
| process minor-fault delta | 130 | 28 |
| process major-fault delta | 0 | 11 |
| output checksum | 73.08446502685547 | 73.08446502685547 |

The remote adapter is about 5.45x slower than the local table at warm p50 in
this unoptimized Python implementation. The setup cost is dominated by creating
the eight RC data sessions. The process fault counters include Python/runtime
activity and are not an RDMA page-count trace; the expected unique table-page
count is the deterministic workload value shown above.

## Correctness result

```text
status=PASS
output_max_abs_error=0.0
output_mean_abs_error=0.0
local_output_checksum=73.08446502685547
remote_output_checksum=73.08446502685547
```

The remote and local DLRM outputs match element-for-element for the measured
batch. This establishes the narrow claim that an official DLRM CPU inference
can consume one embedding table through the existing DMerge demand-paged
mapping.

## Artifact identity and safety

| artifact | SHA-256 or value |
| --- | --- |
| `dmerge_embedding_producer.c` | `fc49cd9aa663c87cd0bc8932d61b54794d6f3b488ca28b25227ba7d9cacfbf28` |
| `dlrm_remote_embedding.py` (measured revision) | `1986a8fc9755129767f47e0791d738d285ad8ddd4796027deaceebac05da5563` |
| `heap.ko` on both guests | `b086fdf840517c7f82e902d8ebe9f563009d2846e329deb4cd354b2bf4a7b69b` |
| PyTorch wheel | `3c99506980a2fb4b634008ccb758f42dd82f93ae2830c1e41f64536e310bf562` |

Both physical Hosts remained up. Only the two SRec guests were restarted to
clear stale RC sessions from an earlier probe; no VF, RNIC, SPD VM, or SLSM VM
configuration was changed. The producer was stopped after the run and the
`heap` modules were left loaded to avoid the known RDMA-CM unload hazard.

## Limitations and next metric

This result does not measure a sustained RDMA bandwidth, concurrent request
scaling, eviction, failure recovery, or an optimized vectorized lookup kernel.
The next useful small-paper experiment is a fixed trace with 1, 2, 4, and 8
unique pages per batch, recording the same setup, cold/warm latency, process
fault deltas, output error, and effective `4096 * unique_pages / cold_time`
page-fetch rate. That can be added without changing the VM topology.
