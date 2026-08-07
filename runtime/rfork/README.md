# rfork userspace runtime

This C11 library is the checked userspace boundary around a Mitosis-compatible
remote-fork device ABI. It validates a fixed role token, preserves ownership of
the kernel device FD, checks raw syscall results, and returns structured audit
state. Its unit test uses injected I/O callbacks and requires no kernel module.

```bash
make test-rfork
```

The library does not contain or emulate `fork.ko`. A successful unit test proves
only the userspace contract; it does not prove remote paging or RDMA execution.
The Mitosis backend identity and dependency boundary are recorded under
`backends/mitosis/`.

Git history identifies the imported implementation and test as Mingxuan Liu's
work, but these C files did not carry SPDX headers in the source repository.
Their public release license remains part of the project-wide ownership review.
