# BlitzScale SPD Two-Guest Model Transfer

Date: 2026-08-25

## Result

The two SPD Guests completed the minimal BlitzScale model-transfer gate. Both
workers started `Inactive`, so neither rank was marked model-ready. Rank 0 then
loaded Llama 3 8B FP16 from its local model file, sent the complete weight
buffer to rank 1, and rank 1 served a request from the received weights.

The run used one MPI rank per Guest, the `eth1` control network, Guest-local
`mlx5_0`, and the previously verified CUDA DMA-BUF MR binary. The
`BLITZSCALE_SKIP_MR` bypass was unset.

Observed sequence:

```text
rank 0: LoadParams(LOAD_FROM_DISK, /models)
rank 0 -> rank 1: SendParams / RecvParams
rank 1: embed ready, then loaded layers 0 through 31
both ranks: WaitRdmaDone completed
rank 1: PrefillV2 and DecodeV2 for request 9001
```

The direct rank-1 request used prompt `Hello`. It returned token IDs and text,
including first token ID `64` (`a`), and completed with generated text. This
confirms that the target could execute after receiving the model from rank 0;
it did not load the model from its own disk during this run.

## Evidence boundary

This result establishes functional cross-Guest model transfer followed by
target inference. It does not yet establish BlitzScale's layer-wise live
execution, because the request was intentionally submitted only after the
complete transfer joined. It also does not claim transfer bandwidth: this was
a correctness gate.

The worker SHA-256 was:

```text
03f784cf1a4d4a336c3f468ff9df9ebed27b58d92f167ded612579ca93f975ee
```

Local raw evidence is under
`/share/home/liumx/tmp/20260825-blitzscale-model-transfer-evidence/` and is not
committed. Temporary MPI credentials were removed after the run.
