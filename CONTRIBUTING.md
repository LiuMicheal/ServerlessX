# Contributing to ServerlessX

ServerlessX is still a private development snapshot under ownership and
licensing review. This guide defines the technical contribution workflow; it is
not a project-wide license or a contributor agreement.

## Useful contributions

- Validate ServerlessPD on a new, isolated GPU/RDMA platform and contribute
  sanitized, machine-readable evidence.
- Improve the portable CPU contract, tests, documentation, deployment checks,
  and failure diagnostics.
- Add an independently owned backend or adapter with complete provenance and
  compatible dependency licenses.
- Improve benchmarks without changing published measurements or presenting a
  build as hardware evidence.
- Prepare Wiseswap, ServerlessRec, or ServerlessLSM code only after its
  ownership, interfaces, dependencies, tests, and evidence are reviewable.

## Before proposing a change

1. Read `README.md`, `docs/status.md`, `AGENTS.md`, and the relevant system or
   profile documentation.
2. Coordinate a narrowly scoped proposal with the repository owner through the
   access channel that granted repository access. Do not imply that GitHub
   Issues, Discussions, or another public channel is active until it is enabled.
3. Identify every source file's author, origin, and license. Do not copy source,
   patch context, binaries, models, containers, or VM images from an external
   academic repository merely because its paper or GitHub page is public.
4. Keep credentials, private topology, internal paths, personal contact lists,
   and unsanitized experiment output outside the repository.

The public contribution channel will be documented here before the repository
is opened. Pull requests cannot resolve missing copyright, institutional, or
dependency rights by themselves.

## Validate a change

Run from the repository root:

```bash
PYTHONPATH=src python3 -m unittest discover -s tests -v
make test-rfork
./sx doctor --format json
./sx plan --profile cpu --format json
./sx run spd --profile cpu --format json
# Verify the exact run_id returned by the preceding command.
./sx verify --run-id RUN_ID --format json
```

Native GDR changes should also compile with `make spd-gdr` on an authorized,
compatible host. Compilation is not evidence of a successful GPU Direct RDMA
transfer.

## Evidence and claims

A contribution must distinguish source availability, a hardware-free test, a
successful build, a bounded hardware run, and a reproducible performance claim.
Include the exact revision, environment, command, run ID, expected result, and
cleanup scope needed to evaluate the claim. Preserve failures rather than
editing result files or selecting an unrelated older run.

## Authorship and review

Keep real Git authorship and upstream attribution. Do not replace an author's
identity with the project name or add people as contributors, mentors, or
institutional supporters without their consent. A maintainer may defer a change
until ownership, licensing, security, or reproducibility questions are resolved.
