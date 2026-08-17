# SPD GPU Remote-Fork Vector-Add Validation

Date: 2026-08-17

## Result

The existing ServerlessPD GPU-aware remote-fork vector-add workload completed
successfully between the source and target SPD Guests. The passing run used
Mitosis remote resume for CPU state, a full-copy CPU snapshot, and PhOS
template/clone-attach for GPU state. Both the restored child and the original
parent completed their post-fork CUDA work and exited with status zero.

This result validates the end-to-end migration path. It does not claim that
the optimized Mitosis COW snapshot path is fully ported to Linux 5.14.

## Reused workload

The run reused the previously validated ServerlessPD workload without changing
its logic:

```text
workloads/vector-add-rfork/vector_add_gpu_rfork.cu
reference revision: 406ef41
ELF SHA-256: 3fbb1a570260d91d71d8070e170d2dee2ed640ff46239289dd8212da3d9de584
```

The executable was a non-PIE CUDA 11 build for the established GPU target. It
exported the expected kernel, contained no stack-protector dependency, and was
byte-identical on both Guests.

## Failed COW run and root cause

The first run used the Linux 5.14 port with `cow,use_rc` and the prior
`ptep_set_wrprotect()` correction. GPU initialization, PhOS template creation,
Mitosis prepare, and remote descriptor retrieval all succeeded. The target
then returned to the wrong user-space continuation and eventually terminated
with SIGSEGV before PhOS clone-attach.

The diagnostic values identify the failure precisely:

```text
saved RIP:                         0x405e57
saved RSP:                         one fixed user-stack address
stack word when snapshot created: 0x406782
stack word fetched by target:      0x4068eb
```

`0x406782` is the return point immediately after the prepare ioctl.
`0x4068eb` is a later return point used after the source continued running.
The target found the correct stack page and completed the remote read, but the
page contents had already been modified by the source. The child consequently
skipped the post-ioctl role check, behaved as a parent, failed the PhOS parent
identity check, and then crashed. The SIGSEGV was a consequence, not the
primary fault.

The 5.14 kernel tracks exclusive anonymous mappings with
`PageAnonExclusive`. The port's COW page wrapper retained the older sequence:

```text
get_page(page)
__page_dup_rmap(page, false)
ptep_set_wrprotect(...)
```

That sequence updates the reference and map counts but does not transition the
5.14 anonymous page out of exclusive ownership. On the next write fault, the
kernel can therefore reuse the same physical page instead of copying it. The
source then overwrites the page referenced by the remote snapshot.

The older successful GPU remote-fork environment used a 4.15 kernel. Its
passing trace also proves that the source parent remained alive: after the
child completed, the parent ran a second counter update and vector-add before
release. The old result was therefore not produced by killing the parent; it
used pre-`PageAnonExclusive` COW behavior.

## Passing full-copy run

For the shortest correctness validation, the second run disabled the `cow`
feature and used the existing Mitosis full-copy snapshot path:

```text
module build features: use_rc
module SHA-256: 45ef4560d67191b61412518ad33d79f66e0a6fb8b8e4ffa695f9e3eb229830ad
```

No workload, PhOS, transport, or vector-add logic changed. The source prepare
copied the CPU pages into an immutable snapshot, while GPU state continued to
use PhOS template/clone-attach. Prepare took 54,794,302 ns for this workload.

The target diagnostic then read the expected stack continuation:

```text
remote stack read: ok
restored stack word: 0x406782
```

The complete validation matrix passed:

| Check | Observed result |
|---|---|
| Source initial counter | `0 -> 1` |
| Source initial vector digest | `ae86a7dbb2b0b4fe` |
| Remote resume | success |
| Child role and machine identity | success |
| PhOS clone-attach | success, `replay_required=0` |
| Child counter | `1 -> 2` |
| Child vector digest, bias 73 | `bbbe0a58ca60cd64` |
| Child exit status | `0` |
| Parent identity after child | unchanged |
| Parent counter | `1 -> 2` |
| Parent vector digest, bias 37 | `6ff608dd06f376a1` |
| Parent exit status | `0` |
| Template manifests | unchanged and identical |
| New kernel fault signatures | none |
| Guest boot identities | unchanged |

The restored child kept the same four GPU virtual addresses as the source,
which is required by the current PhOS replay-free clone-attach contract.

## Interpretation

The SPD port now has a successful GPU-aware vector-add example when CPU state
uses the full-copy Mitosis snapshot. The experiment also separates the GPU
path from the remaining CPU optimization issue: PhOS clone-attach and resumed
CUDA execution work once the CPU snapshot is immutable.

The production COW fix should use the kernel's anonymous-rmap transition under
the required page-table locking and `write_protect_seq` protocol, including an
eager-copy fallback for pinned pages. Merely clearing `PageAnonExclusive` by
hand is not a defensible fix. Until that work is completed, the full-copy mode
is the correctness baseline and the COW mode remains an optimization task.

## Evidence handling

Raw logs, binaries, modules, Guest configuration, and transfer artifacts are
kept in a private local evidence bundle. Its generated 124-file SHA-256
manifest has digest:

```text
c5439f44240543781a5c8032985214c21cc3291ddd838daa67de209604ae35ee
```

Only this sanitized report is committed to the public repository.
