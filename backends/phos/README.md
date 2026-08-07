# PhOS backend

Historical SPD experiments use PhoenixOS for C++ CUDA API interception,
checkpointing, template/clone attach, and VMM restore. PhoenixOS has a
repository-root Apache-2.0 license. The observed ServerlessPD branch contains
19 Mingxuan Liu commits on top of revision `82eb37d`, with roughly 5,584 added
lines across the integration.

Those commits are modifications to an upstream system, not an independently
owned ServerlessX runtime. The complete PhoenixOS tree is not bundled here.
The integration also depends on PhoenixOS-Remoting revision `c46b042`, for which
no repository-root license was observed; that dependency remains a release
blocker.

The Go build scripts and Rust CUDA patcher in the PhoenixOS tree are unchanged
upstream tools. ServerlessX does not claim them as its own Go or Rust core.
