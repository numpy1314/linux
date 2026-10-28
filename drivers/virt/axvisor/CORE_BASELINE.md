# Shared Core baseline

The Linux adapter pins every tgoskits dependency to the unmodified x86
reference revision:

`https://github.com/Ivans-11/tgoskits` at
`f6c897bc10582089bb5b8bb9a06a6d85b84c69dc`.

This is the commit selected by the `ivans/tessera` Linux branch's Cargo.lock.
Use the fixed revision rather than following the moving `axvisor-core` branch.
Do not introduce a patched Core, a path override, or a second ARM Core version.

The earlier irqfd wait/cancel API experiment has been reverted. Its component
tests and reported results do not establish correctness for this baseline.
The existing Core irqfd listener, yield, cancellation-write and join behavior
remain unchanged.

The shared Core already contains runtime VHE detection and ARM KVM support.
However, this Linux ARM adapter still implements the ARM-specific Core hooks
`vhe_host_enter`, `vhe_host_exit`, and `create_user_fd_at`, and its VHE selftest
calls a module absent from the shared revision. Its former `arm-vhe` feature
forwarding therefore cannot be used with the shared revision.

Until the adapter is migrated, `arm-vhe` and `vhe-selftest` are local feature
markers rejected explicitly by the adapter's build script. This permits the
x86 dependency graph to resolve while making the outstanding ARM incompatibility
visible. It is not a working ARM port or an irqfd performance fix. In particular,
removing the build check alone would leave the Linux FPSIMD/CPU/IRQ ownership
boundary unconnected. That boundary must be integrated using the existing
Core interfaces and validated before ARM guest entry is enabled.
