# Add a profile

Add a profile only when the implementation, requirements, claim boundary, and
verifier can be reviewed independently.

1. Create a stable directory under `profiles/portable/` or `profiles/lab/`.
2. Add a human README that states maturity, executable status, prerequisites,
   and explicit non-claims.
3. Add the machine-readable profile declaration with a unique ID, schema
   version, requirements, capabilities, claims, and privileged operations.
4. Add or extend a backend under `src/serverlessx/backends/` and a focused test
   that fails closed when a requirement is absent.
5. Add the profile to the CLI's selection/dispatch table and test both human and
   JSON output.
6. Update provenance and licensing observations for every new external input.
7. Run the full unit suite and a dry plan on a host without the target hardware.

Do not mark `available` true merely because a command can be constructed. The
profile must have a safe, scoped execution path and a verifier that can reject
partial results.
