# SPD 5.14 README Example: CPU ResumeRemote

Date: 2026-08-17 (CST)

This is a one-way CPU canary from the gpu01 SPD Guest to the gpu02 SPD Guest.
The run used the 5.14 port with the `ptep_set_wrprotect()` compatibility fix
and the stale-rseq reset. No physical host reboot or GPU/RDMA discovery was
performed.

## Source and module

- Branch: `diagnose/resume-page-20260814`
- Base: `4d01bfb`
- Diagnostic probe: `b56ab8a`
- Bindgen export fix: `e743e1c`
- Module features: `cow,use_rc` (UD control, RC data)
- Kernel: `5.14.0-687.10.1.el9_8.0.1.x86_64`
- Module SHA-256: `87155f21fc83eaf46728987d52addf42baca638594dfe9a35b42b43cb85fb003`

The module was built in the gpu02 Guest with the vendored Rust dependencies,
`MITOSIS_RDMA_ABI=inbox`, and the matching 5.14 kernel headers. It was loaded
with `mac_id=0,gid_index=3` on gpu01 and `mac_id=1,gid_index=3` on gpu02.

## Example ELF

The three README sources were compiled with GCC 11, C++17, `-O2`,
`-fno-omit-frame-pointer`, `-fstack-protector-all`, `-fPIE -pie`, and the
Guest gflags runtime. The hashes are:

```text
connector-pie-sp     55a40f0659fc2299d17ebe87e5bb0d90ce25a5c96dc3e29a8ebf8c478e7ab9f7
simple_parent-pie-sp 96581445302d6e1c5ec44446bc0e6b5153b27b714897be0debba968bdc135f5f
simple_child-pie-sp  9d9fefa9ce34c41b3351b3928c75d4e6a2754666fda90eeebb543bfba977fdf2
```

The parent ELF is a PIE and contains `__stack_chk_fail`.

## Run

The gpu02 connector used gpu01 Guest GID
`0000:0000:0000:0000:0000:ffff:0a0a:0c74` with `-mac_id=0 -nic_id=0`.
Handler `301` was used once:

```bash
# gpu01
env GLIBC_TUNABLES=glibc.pthread.rseq=0 \
  ./simple_parent-pie-sp -pin=false -handler_id=301

# gpu02, after fork_prepare status=ok
env GLIBC_TUNABLES=glibc.pthread.rseq=1 \
  ./simple_child-pie-sp -mac_id=0 -handler_id=301
```

Each process ran under `script` so `printf` remained line-buffered without an
`LD_PRELOAD` helper.

## Result

- gpu01 parent output: `time 0` through `time 399` (400 lines captured).
- gpu02 resumed child output: `time 0` through `time 395` (396 lines captured).
- Both processes remained alive for more than 30 seconds.
- gpu01 emitted `fork_prepare status=ok` and later
  `fork_unregister status=ok`.
- gpu02 emitted `fork_resume_remote status=ok`.
- No new Oops, BUG, SIGSEGV, or general-protection fault occurred for handler
  `301`.
- gpu02 module cleanup emitted `module_exit status=ok`; gpu01 cleanup also
  emitted `module_exit status=ok`.
- Both Guest boot IDs were unchanged.

The three stack observations were all positive:

```text
parent: stack_hit=true stack_read_rc=0 stack_word=0x55bf0dd6a15f
child descriptor: stack_hit=true page_count=971 vma_count=49
child fault: lookup_hit=true remote_read=ok stack_word=0x55bf0dd6a17f
```

This run disproves a blanket claim that the saved stack page is always omitted
from the descriptor or cannot be fetched remotely. It does not yet prove
bidirectional resume, arbitrary binaries, multithreading, GPU resume, or explain
the earlier handler 173-175 failure; those used different ELF/session state.

Raw stdout, dmesg, process snapshots, and hashes are retained locally in
`ServerlessX-private-evidence/20260817-mitosis-readme-example/`.

## Guest proxy

The current host already has an SSH reverse-forward to the laptop proxy on
`127.0.0.1:7897`. The Guest tunnels were created with:

```bash
ssh -M -S /tmp/spd-gpu01-proxy-7897.sock -fNT \
  -o ExitOnForwardFailure=yes \
  -R 127.0.0.1:7897:127.0.0.1:7897 liumx@10.10.12.116
ssh -M -S /tmp/spd-gpu02-proxy-7897.sock -fNT \
  -o ExitOnForwardFailure=yes \
  -R 127.0.0.1:7897:127.0.0.1:7897 liumx@10.10.12.117
```

With `http_proxy` and `https_proxy` set to `http://127.0.0.1:7897`, both
Guests returned HTTP 200 from GitHub. The tunnel depends on the laptop-side
forward remaining connected.
