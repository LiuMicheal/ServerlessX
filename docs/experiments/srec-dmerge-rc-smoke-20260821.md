# ServerlessRec external RC smoke

Date: 2026-08-21

This is an external lab-evidence record for the ServerlessRec research path.
It does **not** mean that this repository contains an SRec implementation or
that the `ServerlessX` CPU profile can reproduce the run.

## Scope

The experiment used a separately maintained DMerge/Mitosis RC-only probe in
two CPU-only SRec guests. It exercised one remote-heap registration, one
ConnectSession, one descriptor Pull, and one 4 KiB CPU read. DC/DCT, GPU
execution, recommendation inference, and steady-state bandwidth benchmarking
were intentionally out of scope.

The guest software was Rocky Linux 9.8 with kernel
`5.14.0-687.10.1.el9_8.0.1.x86_64`. Both endpoints used the second-RNIC VF,
RoCEv2 GID index 3, and RC data transport. The MN used `mac_id=1` and
`max_core_cnt=16`; the CN used `mac_id=2` and `max_core_cnt=8`.

## Result

The CN test process was pinned to CPU 0. The observed output was:

```text
CONNECT_OK rc=0
PULL_OK rc=0
DMERGE_4K_OK value=0x5352454352444d41 connect_us=81822.709 pull_us=71.565 fault_read_us=19.550
```

The returned magic value matched the producer's registered page. The CN
kernel log also reported an RC data connection with eight sessions. The
`fault_read_us` value is a one-shot functional measurement, not a bandwidth
claim; dividing 4 KiB by it gives only a rough `1.68 Gbit/s` figure dominated
by setup and page-fault overhead.

The loaded `heap.ko` SHA-256 was:

```text
b086fdf840517c7f82e902d8ebe9f563009d2846e329deb4cd354b2bf4a7b69b
```

The source and module remain outside this repository because the repository's
provenance and licensing policy excludes bundling external kernel-module
trees, patches, and generated binaries.

## Safety and boundary

No physical host was rebooted and no RNIC/VF configuration was changed. An
earlier guest-only cleanup attempt used `rmmod heap` while `ib_cm` still had
delayed work queued; that caused an instruction-fetch Oops in the unloaded
module and a guest reboot. Future reruns must use a fresh guest boot or a
proper teardown path rather than unloading the module in place.

This record supports the narrow claim that an external RC-only DMerge probe
completed a one-shot CPU memory smoke in the SRec VM pair. It does not upgrade
the ServerlessX SRec subsystem to executable status.
