# SPD 5.14 P0-P2 progress

Date: 2026-08-10

This note records the public-safe boundary of the current ServerlessPD port.
It does not make the Mitosis or PhOS lab profiles executable.

## Scope

The immediate work is deliberately limited to:

1. freezing the existing GPU/RDMA/PhOS baseline;
2. compiling the external Mitosis backend for the exact SPD guest kernel;
3. running a single-Guest Mitosis runtime gate after the module build is
   accepted.

Cross-guest remote fork, TinyLlama, scheduler work, DCT, and a KRCore rewrite
remain outside this phase.

## P0 evidence boundary

A short canary on the current two SPD guests reproduced ordinary CUDA
DMA-BUF RDMA Write at 98.04 Gb/s in one direction and 98.03 Gb/s in the
other. Both endpoints used `ibv_reg_dmabuf_mr()`.

One archived reverse-direction attempt ended with transport retry exhaustion.
The RDMA links remained active, no test process remained, and the immediate
retry on a new control port completed at 98.03 Gb/s. Both the failed and
successful logs are retained in the private evidence store.

Previously archived evidence was also rechecked:

- bidirectional 2 MiB CUDA VMM DMA-BUF transfer with matching checksums;
- a 2 MiB PhOS-owned direct VMM read with `staging_bytes=0`;
- target clone attach and CUDA digest verification;
- local PhOS checkpoint/restore evidence from both guests.

The cross-guest PhOS source reports a core pass followed by an
abort-after-success teardown. It is not evidence that every teardown path is
already clean.

Raw logs, VM definitions, internal inventory, checkpoint images, binaries,
models, and kernel modules are intentionally excluded from this repository.
They are referenced through private SHA-256 manifests.

## P1 compile gate result

The port did not restart from Mitosis upstream `main`. The preserved pure-RC
integration and the historical attempt21-23 logs were recovered into a clean
worktree. The remaining Linux 4.15 to RHEL/Rocky 5.14 MM work covered
`vm_fault_t`, KABI-wrapped MM/VMA fields, `mm_walk_ops`, page-walk callbacks,
VMA flag helpers, and the Rust-object Kbuild rule.

The clean parent source revision was `1732c276a7e7967b7d4e4a76231b366c0be3f7a7`
with clean KRCore/RDMA dependency revision
`9cd80aef81b75ec81e1bb9a6055355779a80d289`. Against the exact SPD Guest kernel
`5.14.0-687.10.1.el9_8.0.1.x86_64`, the `krdma-test cow use_rc` build with
`--no-default-features` and `MITOSIS_RDMA_ABI=inbox` produced `fork.ko`.

The compile gate passed: modpost reported no unresolved symbols and
`modinfo` reported the exact target vermagic
`5.14.0-687.10.1.el9_8.0.1.x86_64 SMP preempt mod_unload modversions`.
The artifact SHA-256 is
`b3ed2adac39583197c1fba52c44917f2bf6530e5f6a86f1ec7e9232b6dccc8ab`.
The binary, raw logs, VM details, and manifests remain private; only this
sanitized provenance is recorded here. BTF generation was skipped because the
Guest did not expose `vmlinux`.

## P2a runtime gate

The first runtime gate was executed inside one existing SPD test Guest using
the exact kernel `5.14.0-687.10.1.el9_8.0.1.x86_64`. The Guest-built module had
the exact target vermagic and SHA-256
`ffb7ab427db28c3b6001eb48b9137484647314b21a719edc9f0e0c2bb80ef83a`.

The following Guest-side checks passed: `insmod`; RDMA context and RPC thread
initialization (`module_init status=ok`); opening `/dev/mitosis-syscalls`; Nil
ioctl; `fork_prepare` for handler 73 (`descriptor_bytes=25760`) followed by
`fork_unregister`; and `rmmod` (`module_exit status=ok`). The final Guest state
had no loaded module and no Mitosis device node. Host/VM/GPU/RNIC/VF/ACS/IOMMU
configuration and the existing GDR/PhOS baseline were left unchanged, with no
reboot.

This gate emitted one non-fatal kernel warning, `Unpatched return thunk in use.
This should not happen!`; it had no Oops, BUG, panic, or failed unload. A
pre-existing `xpu-server`/`libpos.so` segmentation fault in the Guest dmesg is
retained but is unrelated to this run. The warning means this is not a
warning-free production-ready result.

The result is limited to module/runtime initialization, device ABI access,
prepare/registration, and cleanup. `ResumeLocal` remains unimplemented and no
dual-Guest `ResumeRemote` test was attempted, so this is not evidence of a
complete CPU fork. The next research gate is to resolve or characterize the
return-thunk warning, then run the smallest appropriate remote-fork test.

Private raw logs, the module, VM details, and build patches remain outside this
repository under the lab evidence directory.

Host GPU, VF, ACS, IOMMU, storage, and network configuration are not part of
this single-Guest runtime gate.
