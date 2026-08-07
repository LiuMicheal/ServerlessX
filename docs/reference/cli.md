# CLI reference

The `sx` wrapper runs the repository package without requiring installation.
Every command accepts `--format json` at the end for machine-readable output;
the default `--format text` is a human-oriented summary.

## `profiles`

```bash
./sx profiles [--format text|json]
```

Lists every declared profile and distinguishes host compatibility from whether
the bootstrap actually makes that profile available for execution.

## `doctor`

```bash
./sx doctor [--format text|json]
```

Collects read-only host facts used by profile planning. It must not install,
probe, or mutate a driver, device, service, network, VM, or kernel state.

## `plan`

```bash
./sx plan --profile auto [--format text|json]
./sx plan --profile cpu [--format text|json]
./sx plan --profile cuda_tcp [--format text|json]
./sx plan --profile rdma_dmabuf [--format text|json]
./sx plan --profile phos_lab [--format text|json]
```

Returns the selected profile, compatibility facts, capabilities, steps, and
privileged operations. A plan is not an execution authorization.

## `run`

```bash
./sx run spd --profile cpu [--format text|json]
```

Runs the executable CPU SPD backend and writes a scoped result. Other profiles
are unavailable in this bootstrap and must fail closed. Future commands must
require an explicit profile rather than silently choosing a stronger path.

## `verify`

```bash
./sx verify --latest [--format text|json]
```

Reads the most recent local result and validates its schema and terminal
invariants. Verification does not repair or rewrite a result.

## Exit behavior

Callers should treat a non-zero exit status as no evidence of success. In
particular, an unavailable profile, failed preflight, protocol violation, or
verification failure must remain distinguishable in JSON even if a shell wraps
the command.
