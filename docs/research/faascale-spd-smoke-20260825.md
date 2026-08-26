# FaaScale SPD Guest Single-GPU Smoke

Date: 2026-08-26

## Result

FaaScale completed a minimal isolated inference smoke on the gpu02 SPD Guest.
The run was single-node, single-L20, and non-RDMA; it did not use gpu01 or
modify the existing BlitzScale/SPD processes.

- Source revision: `9db210fcb6979f7c1f73f9819a77e0edb6c5e343`
- Isolated container: `faascale-smoke-gpu02-20260826`
- Model input: the author-provided raw pre-blocked
  `jcbjcc/llama-2-7b`, eight blocks totaling `13,476,831,232` bytes
- Model load completed at `06:28:52`; observed GPU memory was `17,749 MiB`
- Prompt: `I believe the meaning of life is`
- Generation limit: 8 tokens
- TTFT: `30.6 ms`
- Complete request latency: `244.4 ms`
- Worker normal-execute time: `241.6 ms`

The final `FAASCALE_GENERATED_TEXT[...]` marker contained eight unknown-glyph
tokens. This proves that the FaaScale execution path loaded the model and
generated eight tokens, but it does not validate output quality. Tokenizer and
weight compatibility remains an unresolved limitation.

## Token correctness check

A follow-up run printed the generated IDs: `[[0, 0, 0, 0, 0, 0, 0, 0]]`.
The Guest-local tokenizer has a 32,000-entry vocabulary, encodes the prompt as
`[1, 306, 4658, 278, 6593, 310, 2834, 338]`, and decodes ID `0` as `⁇`.
Therefore the glyphs are not a terminal display artifact: the loaded model
actually selected ID 0 at every generated step. The tokenizer file matches the
downloaded input by SHA-256, so the remaining issue is likely the raw weight
block format, model parameter/layout compatibility, or the FaaScale commit;
this smoke is not a valid quality baseline until that is resolved.

## Isolated adjustments

The smoke used only minimal changes in the isolated runtime copy:

1. Change the single-GPU warm-up call to `warm_up(total_gpu_num)`.
2. Call `set_start_method(..., force=True)` for repeatable startup.
3. Set `max_gen_len=8`.
4. Capture generated text with `tokenizer.decode(toks)` and test membership in
   `output.tensors` before emitting the output marker.

The first observer-hook attempt raised a `KeyError`. It was discarded, and the
smoke was rerun cleanly; no number from that attempt is used above.

## Evidence boundary

This result is a functional one-GPU token-generation gate, not a performance
benchmark. It does not validate model quality, RDMA/GDR transport, distributed
execution, or comparison against BlitzScale/SPD. No RDMA device was exposed to
the isolated container.
