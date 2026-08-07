# Profiles

A profile is a versioned declaration of one deployment path. It is not a
wrapper around arbitrary shell commands and it is not proof that a host can
execute the path.

## Important fields

- `id`: stable command-line identifier.
- `tier`: whether the path is portable or tied to a controlled lab.
- `maturity`: executable, source-preview, planned, or reference-only.
- `available`: whether this bootstrap includes an implementation that may be
  invoked by `run`.
- `requirements`: facts that `doctor` can observe or that an operator must
  explicitly provide.
- `capabilities`: what the path could establish when requirements pass.
- `claims`: the narrow statements allowed in result evidence.
- `privileged_operations`: operations requiring separate approval.

`available` is deliberately distinct from host compatibility. A profile can be
described in the repository and still be unavailable; a host can have a GPU
and still fail a profile's exact ABI or driver requirements.

## Selection

`./sx plan --profile auto` considers only profiles marked available, then checks
host facts and declared requirements. It must not infer support from a device
name alone. An explicit unavailable profile must remain unavailable instead of
falling back to an unrelated implementation.

## Maturity vocabulary

| Maturity | Meaning |
| --- | --- |
| `executable` | This checkout contains a runnable implementation with a verifier. |
| `source-preview` | Substantive source and hardware-free tests exist, but deployment gates are incomplete. |
| `planned` | A documented boundary with no executable implementation. |
| `reference-only` | Historical or lab context is documented, while required external material is not distributed. |

Claims should name the maturity and evidence kind. For example, "hardware-free
TinyLlama workload test" is precise; "TinyLlama migration works" is not.
