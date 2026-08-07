# Container deployment

Container deployment is planned but not executable in this bootstrap. No image
is referenced as an implicit download and no container runtime is invoked by
`doctor`, `plan`, or the CPU `run` command.

An executable container profile must pin an image digest, declare mounts and
devices, explain network access, isolate cleanup by run ID, and obtain approval
for privileged flags before starting.
