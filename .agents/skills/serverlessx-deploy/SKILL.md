---
name: serverlessx-deploy
description: Probe a host, select a compatible ServerlessX execution profile, produce a deployment plan, run an approved SPD workflow, and verify its evidence. Use when a user asks to install, deploy, run, reproduce, validate, or troubleshoot ServerlessX or ServerlessPD from this repository.
---

# Deploy ServerlessX

Use the repository's `./sx` interface as the source of truth. Begin with
read-only discovery and leave a machine-readable evidence record for every
execution run. A doctor or plan-only request does not create run evidence.

## Workflow

1. Find the repository root containing `sx` and work from that directory.
2. Read the root `AGENTS.md`, `README.md`, and `QUICKSTART.md`.
3. Run `./sx doctor --format json`. Report failed requirements without changing
   the host.
4. Run `./sx plan --profile <id> --format json` for a user-selected profile, or
   use `./sx plan --profile auto --format json` when none was named. Accept only
   a `ready` plan whose selected profile is both available and runnable. Do not
   treat a documented, planned, source-preview, or reference-only profile as
   executable.
5. Read the selected profile's JSON and README, then read every nested
   `AGENTS.md` that applies to it. Confirm those files agree with the plan.
6. Review the plan. Tell the user about every privileged or host-wide operation
   before requesting approval for it.
7. Run `./sx run spd --profile <id> --format json` only when the user requested
   execution and the plan requires no unapproved changes.
8. Only if step 7 ran in this workflow, verify its returned run ID with
   `./sx verify --run-id <run-id> --format json`. Return the status, run ID,
   profile, claims, and evidence path. On failure, preserve the evidence and
   stop. Never use an unrelated older result as current evidence.

## Safety Rules

- Do not use `sudo`, install packages, load kernel modules, change drivers,
  firmware, IOMMU settings, firewall rules, routes, interfaces, or services
  without explicit user approval.
- Do not stop workloads, VMs, containers, or processes that this run did not
  create.
- Do not expose credentials, private keys, model tokens, internal inventories,
  or local configuration in logs or commits.
- Treat the CPU profile as a contract simulation. Do not describe it as a GPU
  KV-transfer result.
- Fail closed when profile requirements, identities, hashes, ownership, or
  evidence validation do not match.
