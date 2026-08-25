# FaaScale SPD Guest Smoke Preparation

Date: 2026-08-25

## Scope

This is a baseline reproduction setup for FaaScale/LambdaScale on one SPD
Guest. It is deliberately single-node, single-GPU, non-RDMA, and isolated from
the existing BlitzScale and SPD processes. The target is one real prompt that
returns generated text; this note records preparation only and does not claim
that token generation has completed.

## Source and runtime

- Upstream repository: `https://github.com/lambda-scale/lambda-scale.git`
- Observed source revision: `9db210f`
- Runtime copy: Guest-local isolated experiment directory (the upstream
  checkout was not modified)
- Existing image reused: `blitzscale:spd-l20-runtime-20260824`
- Guest venv packages supplied through `PYTHONPATH`; CUDA driver libraries and
  only the assigned L20 device are exposed to the new container.
- No RDMA device or InfiniBand sysfs tree is mounted for this smoke.

## Runtime-only adjustments

The isolated copy contains only the changes needed for a one-GPU smoke:

1. Replace two non-RDMA `warm_up(2)` calls with `warm_up(total_gpu_num)`.
2. Preserve decoded LLM text in the returned intermediate result and print a
   `FAASCALE_GENERATED_TEXT[...]` marker at the controller.
3. Set the first-run generation limit to 8 tokens.
4. Set `total_node_num=1`, `total_gpu_num=1`, `is_rdma=False`,
   `model_name=llama-2-7b`, and a one-entry local node configuration.

The model storage directory is an isolated runtime directory. It is currently
empty because the eight author-provided `jcbjcc/llama-2-7b` `.pth` shards have
not yet been placed on the Guest.

## Checks completed

- Assigned L20 was idle before and after setup.
- CUDA/PyTorch canary in the new container: one L20 visible and usable.
- FaaScale dependencies imported successfully: PyTorch `2.5.1+cu121`,
  pyzmq `25.1.1`, Transformers `4.42.0`, FairScale `0.4.13`, and the compiled
  `ipc_p2p` extension.
- Manager-only check opened its execute/transfer listeners on ports 8000 and
  9000, then exited cleanly; those ports are free again.
- No FaaScale worker, model loader, or inference request was started.
- Existing BlitzScale containers and SPD processes were not stopped or
  modified.

## Blocker and next gate

The requested token smoke cannot be run until all eight `.pth` shards are
available in the isolated model-storage directory. Once present, the remaining
sequence is: start the new worker, start the manager, send `Start` and
`DeployModel`, wait for block loading, send one short prompt, capture the
`FAASCALE_GENERATED_TEXT[...]` line and timing, then remove only the new
FaaScale container.

Until that gate passes, this setup must be reported as **prepared/blocked on
model input**, not as a successful FaaScale inference reproduction.
