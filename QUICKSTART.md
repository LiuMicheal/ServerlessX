# ServerlessX quickstart

This guide runs the portable SPD contract simulation. It is the only execution
path declared ready in the private bootstrap.

## Prerequisites

- Linux or macOS shell
- Python 3.9 or newer available as `python3`
- A local checkout of this repository

No GPU, model download, package installation, container runtime, root access,
or network access is required.

## 1. Inspect the host

```bash
./sx doctor
```

`doctor` is read-only. It reports relevant host capabilities without changing
drivers, devices, services, network configuration, or files outside the
repository.

For machine-readable output:

```bash
./sx doctor --format json
```

## 2. Review the plan

```bash
./sx plan --profile cpu
```

The plan should identify the portable CPU profile and list no privileged
operation. Planning is read-only and creates no run evidence.

## 3. Run the SPD contract simulation

```bash
./sx run spd --profile cpu
```

The command validates fixed TinyLlama experiment metadata, advances the SPD
acknowledgement sequence, checks ownership transitions, and writes one result
under `runs/<run-id>/result.json`.

This is deliberately a contract simulation. A successful result is not a CUDA,
TCP KV-transfer, RDMA, model-quality, latency, throughput, or PhoenixOS result.

## 4. Verify the evidence

```bash
./sx verify --latest
```

Verification reads the most recent result and checks its structure and terminal
state. Keep the reported result path when filing an issue.

## 5. Run the tests

```bash
PYTHONPATH=src python3 -m unittest discover -s tests -v
make test-rfork
```

The tests cover the SPD contract, TinyLlama prefill/decode workload, and C
remote-fork userspace boundary with fake runtimes, local sockets, and injected
I/O. They do not require PyTorch, Transformers, CUDA, a model checkpoint, or
`fork.ko`. The C test requires a Linux C compiler and GNU Make.

## Let the repository choose

```bash
./sx plan --profile auto
```

`auto` may select only a profile that is both marked executable and compatible
with the observed host. In this bootstrap, that normally means `cpu`. An
unavailable hardware profile must fail closed rather than falling through to
an unverified deployment.

## Troubleshooting

If `./sx` is not executable, run it through the shell without changing system
state:

```bash
bash ./sx doctor
```

If `python3` is older than 3.9, use a newer interpreter explicitly. Do not add
compatibility packages to the system Python for the CPU path.

If a command fails, preserve its output and any new `runs/<run-id>` directory.
Do not claim success from a partial result, and do not hand-edit result JSON.

Hardware paths are intentionally separate. Continue with
[docs/how-to/hardware-profiles.md](docs/how-to/hardware-profiles.md) only after
the CPU path is understood.
