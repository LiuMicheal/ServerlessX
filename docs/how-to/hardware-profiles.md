# Hardware profile review

Hardware deployment is intentionally a separate workflow from the CPU
quickstart. Before asking an agent or operator to run one:

1. Read the profile README and adjacent JSON declaration.
2. Run `./sx doctor --format json` and save the output outside the repository if
   it contains private topology.
3. Run `./sx plan --profile <id> --format json`.
4. Review every privileged operation, target identity, timeout, and cleanup
   action. Obtain explicit approval for each operation.
5. Confirm that all required external inputs are lawfully available and pinned
   by revision/hash.
6. Run once with an explicit run ID and verify the result before drawing a
   research conclusion.

## CUDA/TCP

The CUDA/TCP source path needs a compatible CUDA/PyTorch/Transformers runtime,
TinyLlama model and tokenizer material, and a process lifecycle that can cleanly
stop both roles. The bootstrap does not download or install these inputs.

## RDMA DMA-BUF

The included native source can be compiled without running it:

```bash
make spd-gdr
```

The native RDMA deployment additionally needs a kernel and driver combination that
supports both CUDA DMA-BUF export and the selected RDMA provider, an authorized
device/GID, exact ABI-compatible libraries, and isolation from other jobs. A
successful build or a device listed by `lspci`/`nvidia-smi` is insufficient
execution evidence.

## PhoenixOS lab

The PhOS path is controlled-lab work. Follow
`profiles/lab/phos/AGENTS.md`, inventory the exact VMs and daemon start ticks,
and preserve all pre-run hashes. The public bootstrap has no code or artifact
that can launch this path.
