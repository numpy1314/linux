# Shared Core baseline

For the x86-product/native-KVM/ARM comparison and the evidence required before
further adapter changes, see [ARM64_KVM_COMPARISON.md](ARM64_KVM_COMPARISON.md).

The Linux adapter pins every tgoskits dependency, for both x86 and ARM, to
one shared revision:

`https://github.com/numpy1314/tgoskits` at
`ce800d4d28fd552441f166c2679f63f8b14d121c`.

This revision contains six commits on top of
`4152c3e780feb5e44241fb3fa143fe04bdca0181`, the original x86 reference selected
by the `ivans/tessera` Linux branch's Cargo.lock on 2026-10-30 (Linux revision
`e725a24a0ec62b97a5a7476f102aba268a7682aa`). The additions cover ARM firmware
call isolation and offline CPU reset, the shared control entry/stop contract,
ARM PSCI lifecycle handling, the supported preferred-target profile, the
stop/signal-only wait interface, and ARM system-event data discovery.
The comparison document preserves the original four-way audit as a fixed
historical snapshot and records this implementation separately.
Use the fixed revision rather than following the moving `axvisor-core` branch.
Do not introduce a path override or a second, ARM-only Core version.

The earlier irqfd wait/cancel API experiment has been reverted. Its component
tests and reported results do not establish correctness for this baseline.
The existing Core irqfd listener, yield, cancellation-write and join behavior
remain unchanged.

The inherited 4152 implementation already unmaps an eagerly pinned memory slot
as one GPA range before releasing its pinned pages and address-space reference.
The previous
`f6c897bc10582089bb5b8bb9a06a6d85b84c69dc` baseline unmapped it page by page.
The shared implementation is used without an ARM-only teardown patch. This
source-level improvement does not establish Linux ARM runtime correctness.

## Linux ARM VHE ownership

The adapter uses the shared Core APIs, including the new ARM control-entry,
synchronous kick and interruptible-wait contract. The old ARM-only
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

`axvisor_linux.watchdog=0` disables that periodic hrtimer for focused entry/stop
validation; it does not disable the guest virtual-timer PPI. The default is
`watchdog=1`. System-event stopping uses the Core's entry gate and a synchronous
Linux IPI acknowledgement, independently of the periodic exit budget. This
contract must also hold for a guest that masks its own IRQs and never exits
voluntarily.

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

## ARM PSCI lifecycle contract and remaining limits

The immediate Firecracker scope is accurate OFF/RESET reporting, guest firmware
isolation, and VMM-driven stop, KVM_RUN interruption, thread join and resource
release. Firecracker does not require a separate all-vCPU-stopped notification
before starting that exit sequence. The synchronous system-stop behavior below
is a retained extension with its own validation cases. Timer-aware suspend and
full VM reset are not prerequisites for this Firecracker exit path.

The shared ARM backend now forwards HVC/SMC PSCI requests through the existing
`Hypercall` exit to the selected frontend. It no longer forwards unhandled
guest SMC requests to physical firmware. Unsupported PSCI functions, unknown
SMC functions and unsupported SMC immediates return `NOT_SUPPORTED` (-1).
Non-PSCI private HVCs retain their previous dispatch. There is no new public
exit enum, and the x86 `SystemDown` to `KVM_EXIT_SHUTDOWN` mapping is unchanged.

In the ARM KVM control frontend, `SYSTEM_OFF` and `SYSTEM_RESET` produce
`KVM_EXIT_SYSTEM_EVENT`, with shutdown/reset type respectively, `ndata=1` and
`data[0]=0`. Before returning the event, Core publishes STOPPED for every vCPU
in the VM, cancels pending CPU starts, and synchronously kicks active runners.
The entry gate is checked at every actual guest entry, including internal
re-entry loops. The originating x0 is set to `INTERNAL_FAILURE` (-6), matching
native KVM's behavior if userspace explicitly resumes the stopped context.
A subsequent `KVM_RUN` of a STOPPED vCPU waits interruptibly; it does not itself
restart or rebuild the VM. No adapter callback powers off or resets the host
on behalf of these guest requests.

`CPU_OFF` enters STOPPED. `CPU_ON` queues a reset for the target runner, which
resets its architectural state before entry. `GET_ONE_REG` and `SET_ONE_REG`
also consume a pending reset before accessing registers, as in native ARM
KVM. For valid startup arguments, a runnable or already queued target returns
`ALREADY_ON` (-4); an invalid or uninitialized target returns `INVALID_PARAMS`
(-2). A concurrent system
stop prevents an older source `CPU_ON` from restarting another vCPU. This
backend requires a four-byte-aligned entry PC and rejects an unaligned PC
before queueing; Linux 6.12 native KVM does not perform this upfront check.

The current profile reports PSCI 0.2 and implements VERSION, CPU_OFF, CPU_ON,
AFFINITY_INFO, MIGRATE_INFO_TYPE, SYSTEM_OFF and SYSTEM_RESET. It is a limited
lifecycle profile, not a claim of complete PSCI 0.2 or ARM KVM ABI support.
`KVM_ARM_PREFERRED_TARGET` includes `KVM_ARM_VCPU_PSCI_0_2`, which the initial
`KVM_ARM_VCPU_INIT` requires; `POWER_OFF` controls the initial MP state. The
PSCI version one-reg interface only accepts 0.2. Repeated INIT, CPU_SUSPEND,
PSCI_FEATURES, RESET2 and full VM reconstruction in place are not supported
in this batch. Timer-aware suspend and repeated INIT/cache reset remain
separate work. The static frontend retains its basic CPU_ON/CPU_OFF/poweroff
uses and is not the ARM KVM control implementation.

`SET_MP_STATE` is allowed concurrently with an active `KVM_RUN` and coordinates
through the entry gate and kick/wake protocol. This is broader asynchronous
control than Linux 6.12 native KVM, which serializes that ioctl with the vCPU
mutex. It must not be described as identical native ioctl semantics. Whole
ioctl migration pinning, WFI/idle handling and irqfd listener differences also
remain as described in the comparison document.

The former HVC RESET and physical-SMC-forwarding blockers describe the old
4152 snapshot, not this implementation. Validate Firecracker exit/join/cleanup
and the retained synchronous-stop extension separately, first in isolated
QEMU and then on the board. This document records no runtime result or board
admission verdict.

## Reproducible checks

- `tests/eventfd/`: actual Linux eventfd readiness, cancellation and reference
  lifecycle, on ARM and x86, including an old-reader negative control.
- `tests/vhe-simd/`: real Linux kernel SIMD ownership through the Rust callback
  scope, scheduling and a disabled-handoff negative control.
- `tools/axvisor/tests/arm64-shared/`: shared-Core VM creation, failed device
  copyout rollback, MMIO exit, memory teardown and fd balance.
- `tools/axvisor/tests/arm64-psci/`: HVC/SMC system-event ABI, unsupported calls,
  CPU_ON register ordering, synchronous all-vCPU stop and signal interruption;
  the runner defaults to `axvisor_linux.watchdog=0`.
- `tools/axvisor/tests/arm64-linux-guest/`: real Firecracker/Linux CPU, timer,
  block I/O, poweroff and separately recorded restart behavior.

Build and result directories belong outside the source tree. Record the exact
host Image/config, test assets and command line for each run. QEMU checks do
not replace RK3588 board validation, prove absence of every memory leak, or
establish MiBench/lmbench performance.
