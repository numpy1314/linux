# Shared Core baseline

For the x86-product/native-KVM/ARM comparison and the evidence required before
further adapter changes, see [ARM64_KVM_COMPARISON.md](ARM64_KVM_COMPARISON.md).

The Linux adapter pins every tgoskits dependency to the unmodified x86
reference revision:

`https://github.com/Ivans-11/tgoskits` at
`4152c3e780feb5e44241fb3fa143fe04bdca0181`.

This is the commit selected by the `ivans/tessera` Linux branch's Cargo.lock
when checked on 2026-10-30 (Linux revision
`e725a24a0ec62b97a5a7476f102aba268a7682aa`).
Use the fixed revision rather than following the moving `axvisor-core` branch.
Do not introduce a patched Core, a path override, or a second ARM Core version.

The earlier irqfd wait/cancel API experiment has been reverted. Its component
tests and reported results do not establish correctness for this baseline.
The existing Core irqfd listener, yield, cancellation-write and join behavior
remain unchanged.

This revision already unmaps an eagerly pinned memory slot as one GPA range
before releasing its pinned pages and address-space reference. The previous
`f6c897bc10582089bb5b8bb9a06a6d85b84c69dc` baseline unmapped it page by page.
The shared implementation is used without an ARM-only teardown patch. This
source-level improvement does not establish Linux ARM runtime correctness.

## Linux ARM VHE ownership

The adapter now uses the shared Core's existing APIs. The old ARM-only
`vhe_host_enter`, `vhe_host_exit`, `create_user_fd_at` and `vhe-selftest`
dependencies have been removed. `arm-vhe` is a Linux adapter feature, not a
second Core implementation. The builder always uses the locked dependency
graph and has no local Core override option.

The admitted profile is Linux 6.12 or newer with schedulable kernel FPSIMD,
4 KiB pages, Linux at EL2, DT GICv3, and exclusive ownership of the virtual GIC
interface. Native KVM, CPU hotplug, pseudo-NMI, SME, MTE and POE must be disabled.
Online Linux CPU IDs must be dense starting at zero. The Core stage-2 page-table
level must match the host PA range (three levels for the 40-bit RK3588/QEMU
Cortex-A76 profile; `ept-level-4` for a corresponding wider PA profile).
Admission is checked before publishing `/dev/kvm`. The direct static-VM boot
path remains unsupported; use `axvisor_linux.control=1`.

The Linux ioctl wrapper owns kernel SIMD while `KVM_RUN` is active. It permits
sleep and scheduling; the Core's existing `with_bound_vcpu` / `IrqSave` protects
each bounded guest-switch batch and restores host state before releasing IRQs.
The Rust domain uses the softfloat, `target_os=none` target. Calls back into
sleepable Linux code temporarily release SIMD ownership using a Rust RAII
scope, so callbacks may themselves use kernel NEON. IRQ-disabled callbacks
retain ownership. This does not establish handling of every possible FIQ,
SError or debug exception during a world switch.

Linux owns physical interrupt acknowledgement and completion. `fetch_irq`
reads `ICC_HPPIR1_EL1` without acknowledging the IRQ, so Core can classify a
host IRQ exit and reach its scheduling point even with `PREEMPT_NONE`. The
peeked INTID is not dispatched through Core's software IRQ registry. The adapter
registers the guest virtual-timer PPI and runs a 5 ms hrtimer only on CPUs with
an active `KVM_RUN`. Migration is pinned for the ioctl; concurrent vCPU tasks
on the same CPU share a reference-counted timer/PPI activation. The last
run on that CPU cancels the timer and disables the PPI. Host-only CPUs are not
woken for another CPU's guest. The hrtimer provides an exit opportunity even when a guest
does not program a timer; it is not a guarantee that `KVM_RUN` returns to
userspace within 5 ms. The unmigrated direct-VM timer-injection callback remains
an explicit unsupported path, not a silent no-op.

Set `axvisor_linux.guest_cpus=4-7` for the initial RK3588 A76 pool, and set the
whole VMM process's affinity to that pool before any control ioctl. The default
pool is all online CPUs, which will be rejected on a heterogeneous system.
Common VHE/PA/GIC capabilities are checked on every online CPU; the raw guest
CPU profile is sampled on the first pool CPU and compared across the pool.
Core per-CPU storage still covers all online Linux CPUs with their original
IDs. No CPU renumbering is performed.

Every control ioctl disables migration and checks pool membership (`-EXDEV`
outside the pool), including VM creation because Core creates architectural
vCPU state there. Scheduling and sleep remain allowed. Separate ioctls may
run on different pool CPUs, but migration inside a single `KVM_RUN` is not
supported. An external affinity change that excludes the current CPU can
therefore wait until the ioctl returns. File release and irqfd worker teardown
are not restricted to the guest pool. This is a functional validation profile,
not native KVM scheduling equivalence.

`KVM_CREATE_DEVICE` reserves its Linux file descriptor until Core's usercopy
succeeds. On failure, the adapter discards the unpublished Linux file and fd
reservation while respecting Core's ownership of the device id. This uses the
existing `create_user_fd` API and does not require a Core extension.

Initialization failures before the Core thread starts release the adapter's
per-CPU storage, MMIO mappings and owned ARM FDT copy. Failures after Core
initialization begins retain possibly referenced memory until reboot; only
the adapter endpoints and VHE PPI registration are withdrawn. There is no
claim of a complete Core shutdown/unload implementation.

## Known functional boundary

The fixed revision's HVC PSCI `SYSTEM_RESET` request is not implemented by the
shared KVM frontend. `arm_vcpu::handle_psci_call` leaves it as a `Hypercall`;
`axvisor_core::kvm::run::arm64::handle_psci_hypercall` returns `NOT_SUPPORTED`
and the `KVM_RUN` loop continues. None of the existing host callbacks exposes
that request's function number. Translating a later guest halt, console line
or signal into a successful reset would hide this missing behavior and is
not done by the adapter.

`SYSTEM_OFF` follows the shared `SystemDown` path and currently produces
`KVM_EXIT_SHUTDOWN`, which Firecracker accepts. This is distinct from the
native ARM KVM `KVM_EXIT_SYSTEM_EVENT` shutdown/reset ABI. Successful poweroff
and resource cleanup do not establish restart support or complete KVM ABI
compatibility. Keep the restart test failing until the shared Core contract
supports the required exit; do not substitute a private ARM Core patch.

The SMC conduit has an additional board-validation blocker. Core sets
`HCR_EL2.TSC`, traps the guest SMC, and first checks `handle_psci_call`.
Because `SYSTEM_RESET` is not handled there, `handle_smc64_exception` forwards
the request through a real `smc #0` with the guest's arguments. On a board
whose EL3 firmware implements that PSCI request, guest code could therefore
reset the physical host. `SYSTEM_OFF` is intercepted before that fallback.
This is a source-level finding, not an intentional board-reset experiment.
It also applies when the normal guest DT selects HVC: guest EL1 code can
still execute SMC. The existing adapter callbacks cannot intercept this
path. Do not boot this candidate on the board as a completed isolation
implementation; resolve the shared Core PSCI contract before board admission.

## Reproducible checks

- `tests/eventfd/`: actual Linux eventfd readiness, cancellation and reference
  lifecycle, on ARM and x86, including an old-reader negative control.
- `tests/vhe-simd/`: real Linux kernel SIMD ownership through the Rust callback
  scope, scheduling and a disabled-handoff negative control.
- `tools/axvisor/tests/arm64-shared/`: shared-Core VM creation, failed device
  copyout rollback, MMIO exit, memory teardown and fd balance.
- `tools/axvisor/tests/arm64-linux-guest/`: real Firecracker/Linux CPU, timer,
  block I/O, poweroff and separately recorded restart behavior.

Build and result directories belong outside the source tree. Record the exact
host Image/config, test assets and command line for each run. QEMU checks do
not replace RK3588 board validation, prove absence of every memory leak, or
establish MiBench/lmbench performance.
