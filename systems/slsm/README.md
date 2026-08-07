# SLSM

SLSM is a reserved ServerlessX subsystem boundary for serverless lifecycle and
state management: placement, resource ownership, state transitions, and policy.
This bootstrap does not include an SLSM implementation or a runnable profile.

The boundary is intentionally separate from SPD's fixed experiment contract.
Future code should consume versioned interfaces rather than copy SPD internals
or assume a particular PhoenixOS, Remoting, KRCore, or RDMA implementation.
