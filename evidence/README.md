# Evidence policy

`evidence/` documents how a result can be made reviewable. Generated local run
output belongs under `runs/` and is ignored by Git.

An evidence bundle should be:

- tied to one run ID and source revision;
- sanitized of credentials, private topology, absolute home paths, and model
  data that the publisher cannot redistribute;
- accompanied by machine-readable status and hashes;
- explicit about whether it is a simulation, hardware-free test, bounded
  hardware run, or independently reproducible deployment;
- preserved on failure rather than rewritten into a success.

Do not commit raw VM images, kernel modules, DSOs, model checkpoints, packet
captures, private inventories, or unreviewed logs. A checksum is evidence of
identity, not a permission to publish the underlying file.
