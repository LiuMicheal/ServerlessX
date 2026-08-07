# External backends

ServerlessX separates its owned control/data-plane code from research systems
used by historical experiments. A backend record identifies an upstream and a
pinned revision; it does not vendor source, grant a license, or make the backend
part of the portable release.

- `mitosis/`: Rust remote-fork kernel backend used to build `fork.ko`.
- `phos/`: C++ GPU checkpoint/restore and clone-attach backend.

Both remain unavailable to `./sx run`. Their dependency and license blockers
must be resolved independently of the portable CPU and native GDR code.
