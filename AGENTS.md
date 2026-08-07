# Repository instructions

These instructions apply to humans and coding agents working anywhere in this
repository. A more specific `AGENTS.md` may add constraints below its directory.

## Begin safely

1. Read `README.md` and `QUICKSTART.md`.
2. Run `./sx doctor --format json`; this must remain read-only.
3. Run `./sx plan --profile <id> --format json` and inspect its status,
   requirements, capabilities, claims, steps, blocked reason, and privileged
   operations. Inspect output paths when an execution command reports them.
4. Read the selected profile README and JSON plus any applicable nested
   `AGENTS.md`. Confirm they agree with the plan.
5. Run only a profile whose JSON marks it executable and whose requirements
   pass. Fail closed on missing, ambiguous, or stale facts.
6. After a run, verify the run ID returned by that command and report its result
   path. Do not verify an unrelated older run as evidence for the current work.

The profile JSON is the machine-readable source of truth. Documentation must
not upgrade a profile's maturity or executable status.

## Permissions

- `doctor`, `plan`, tests, source inspection, and reads below this checkout are
  allowed by default.
- Do not invoke `sudo`, load or unload kernel modules, install or replace GPU or
  RDMA drivers, change IOMMU settings, alter network interfaces or routes,
  start or stop shared services or VMs, or modify firmware without explicit
  approval for that exact action and target.
- Do not download models, source trees, packages, containers, or binaries
  unless the user explicitly approves the network and storage effects.
- Never write credentials, private keys, tokens, passwords, host-specific
  secrets, or private inventory to the repository or run evidence.
- Never clean up processes, files, devices, containers, or VMs that were not
  created by the current run. Cleanup must be scoped by the run ID.
- Treat `profiles/lab/phos/AGENTS.md` as an additional mandatory policy for the
  PhOS lab profile.

## Layout

- `src/serverlessx/`: implementation; keep the portable path Python 3.9+.
- `tests/`: standard-library `unittest` suite.
- `profiles/`: machine-readable declarations with adjacent human guidance.
- `systems/`: subsystem scope and claim boundaries.
- `testkit/`: schemas and verification data.
- `runs/`: generated local evidence; never commit it.
- `provenance/`: audited upstream identity and licensing observations.
- `.agents/skills/`: agent workflow, subordinate to this file.

## Development rules

- Preserve historical wire schemas beginning with `serverlesspd.`. A breaking
  schema change requires a new version and an explicit converter.
- Keep the CPU path free of third-party runtime dependencies.
- Use normal package imports and `importlib.resources` for packaged data; do
  not depend on the checkout's absolute path.
- Use documentation-only IP addresses from RFC 5737 in fixtures and examples.
- Do not copy external source, patches, generated files, model weights, kernel
  modules, shared libraries, VM images, or containers into this repository.
- Update `provenance/upstreams.json` when an upstream identity changes. A URL
  is attribution, not permission to redistribute.
- Do not add a project-wide license until copyright and institutional ownership
  review is complete and the repository owner explicitly approves it.
- Make claims narrowly: distinguish a simulation, a hardware-free test, a
  hardware run, and a reproducible deployment.

## Validate changes

Run from the repository root:

```bash
PYTHONPATH=src python3 -m unittest discover -s tests -v
./sx doctor --format json
./sx plan --profile cpu --format json
./sx run spd --profile cpu --format json
# Replace RUN_ID with the run_id returned by the preceding command.
./sx verify --run-id RUN_ID --format json
```

Also inspect the proposed change for absolute home paths, internal addresses,
credentials, private-key markers, binaries, and accidentally bundled upstream
material.

## Definition of done

A change is complete only when relevant tests pass, declared commands match
actual behavior, a run either verifies or clearly fails, no capability claim
exceeds its evidence, generated evidence stays untracked, and provenance plus
licensing boundaries remain accurate.
