# Getting started

ServerlessX separates observation, planning, execution, and verification:

```text
doctor (read-only) -> plan (read-only) -> run (writes scoped evidence) -> verify
```

This separation lets a person or coding agent understand a deployment before it
changes state.

## First run

From the repository root:

```bash
./sx doctor --format json
./sx plan --profile auto --format json
./sx run spd --profile cpu --format json
./sx verify --latest --format json
```

The first bootstrap has one executable profile, `cpu`. `auto` therefore chooses
it when its interpreter requirements pass. The output of `plan` is the review
point: it names the profile, capabilities, requirements, steps, and privileged
operations before `run` starts.

## Files written by a run

Runs are scoped below `runs/<run-id>/`. A run should contain at least:

- `result.json`: terminal status, profile ID, claims, and verification fields;
- any small deterministic event or digest files needed to verify that result.

Run IDs are not licenses or publication approvals. Keep local results out of
Git unless a separate evidence review explicitly approves a sanitized bundle.

## A useful failure

An unavailable CUDA, RDMA, or PhOS profile should produce a clear plan failure
or an unavailable status. Do not use `--profile cpu` after such a failure and
describe the result as hardware evidence. The CPU simulation answers a smaller
question and is useful precisely because its claim boundary is explicit.

## Tests before code changes

```bash
make test
```

The portable test suite uses the Python standard library plus a Linux C compiler
and GNU Make. It does not require `fork.ko`, CUDA, or RDMA. External model and
hardware frameworks are not silently installed by the CLI.
