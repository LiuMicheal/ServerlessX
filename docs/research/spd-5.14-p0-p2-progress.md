# SPD 5.14 P0-P2 progress

Date: 2026-08-11

This note records the public-safe boundary of the current ServerlessPD port.
It does not make the Mitosis or PhOS lab profiles executable.

## Scope

The initial P0-P2a work was deliberately limited to:

1. freezing the existing GPU/RDMA/PhOS baseline;
2. compiling the external Mitosis backend for the exact SPD guest kernel;
3. running a single-Guest Mitosis runtime gate after the module build is
   accepted.

At that checkpoint, cross-guest remote fork, TinyLlama, scheduler work, DCT,
and a KRCore rewrite were outside the phase. P2b and P2c below extend only to
the smallest dual-Guest CPU `ResumeRemote` gate.

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

At that point, the result was limited to module/runtime initialization, device
ABI access, prepare/registration, and cleanup. `ResumeLocal` remains
unimplemented and no
dual-Guest `ResumeRemote` test was attempted, so this is not evidence of a
complete CPU fork. The next research gate is to resolve or characterize the
return-thunk warning, then run the smallest appropriate remote-fork test.

Private raw logs, the module, VM details, and build patches remain outside this
repository under the lab evidence directory.

Host GPU, VF, ACS, IOMMU, storage, and network configuration are not part of
this single-Guest runtime gate.

## P2b dual-Guest checkpoint

The dual-Guest `ResumeRemote` gate is now scoped to a minimal RoCEv2
compatibility fix in the external KRCore dependency. Ordinary userspace RC
traffic between the two SPD Guests succeeds with the IPv4-mapped RoCEv2 GID,
but inherited kernel control-path code assumed GID index 0, rejected a legal
`ffff` GID hextet, and converted that parse error into a kernel panic. The
work-in-progress patch makes the GID index configurable and turns malformed
GIDs into normal connection failures.

Comparison against an independently exercised 112/113 KRCore/KRSN setup also
confirmed the remaining narrow porting requirement: exact GID-attribute
lifetime management, RoCE CM attributes, a fallback path when subnet-admin
lookup is unavailable, and RoCE-aware RC/UD address-vector construction.
At this P2b checkpoint, no complete dual-Guest CPU fork or GPU remote fork was
claimed. No module from that work-in-progress source had been built or loaded,
and Host/VM device configuration remained unchanged.

## P2c dual-Guest CPU ResumeRemote

The minimal one-way CPU `ResumeRemote` gate now passes between the two SPD test
Guests on the exact kernel
`5.14.0-687.10.1.el9_8.0.1.x86_64`. The reviewed module completed the RoCEv2
data and control connection, source prepare, target remote resume, and normal
on-demand page fetching. The final candidate SHA-256 is
`a041dd55858a217010729aecdcacd29b587f5c529c756bebafe18acc0f089c24`;
modpost passed, dependencies are `ib_core,ib_cm`, and the module reports the
exact target vermagic. BTF was skipped because the Guest does not expose
`vmlinux`.

The immediate post-resume SIGSEGV was not a missing saved-RIP page. Replacing
the target address space left the target thread's kernel rseq registration
pointing into its discarded TLS. The 5.14 notify-resume path touched that stale
pointer before the first Mitosis page fault. The minimal compatibility fix
uses the kernel's existing exec-style rseq reset before replacing the old
address space. An A/B run then removed the temporary RIP bootstrap: the saved
RIP and stack were fetched through the normal remote page-fault path, so no
bootstrap remains in the final implementation.

The final canary starts the source process with glibc rseq disabled from exec
and leaves rseq enabled in the target launcher. The target observed and reset
a nonzero old registration, returned to the restored source image, and stayed
alive. At the first live observation, source and target were both in syscall
230 (`clock_nanosleep`) with identical syscall arguments, stack pointer, and
instruction pointer; both remained alive in the same syscall three seconds
later. Cleanup left no test process. Host/VM/GPU/RNIC/VF/ACS/IOMMU and scheduler
configuration were unchanged, and neither Host nor Guest was rebooted.

This is intentionally a narrow result: one direction, one single-thread
canary, and source rseq disabled for the whole process lifetime. It is not a
general rseq migration solution, a bidirectional/stress/failure-recovery test,
GPU remote fork, TinyLlama, or end-to-end ServerlessPD. `ResumeLocal` remains
unimplemented. The existing 98 Gb/s DMA-BUF result is an independent GDR
baseline and is not a CPU remote-fork performance result.

One unload-lifecycle limitation also remains. A server-first module unload
with live RC/CM sessions can race an asynchronous callback. Controlled tests
therefore stop exact test processes, unload the client first, wait, and only
then unload the server. The final reviewed-module reload used that order.

External source patches, raw logs, VM inventory, internal addresses, binaries,
and kernel modules remain in the private evidence store and are not published
in this repository.
