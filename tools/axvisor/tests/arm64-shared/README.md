# Shared-Core ARM64 control smoke

This is a functional bring-up test for the Linux hosted Core, not a native KVM
test or a Linux-guest/performance benchmark. The kernel configuration and log
checks require the AxVisor control endpoint and reject native KVM. Kernel
build provenance must be recorded by the caller; a configuration file alone
does not prove which source produced an arbitrary Image.

The freestanding `/init` defaults to three complete VM lifecycles. Each round creates
a VGICv3, checks that a read-only `KVM_CREATE_DEVICE` output page returns
`EFAULT` without installing an fd or leaking a reserved fd slot, then retries
creation in the same VM. Both the installed-fd count and lowest available fd
number are checked around this failure.
It maps guest RAM (one 4 KiB page by default), initializes one vCPU, and executes `str w1, [x0]`
to require the expected MMIO address, size, direction, and payload. It closes
the run mapping and all VM/device/vCPU fds and requires the fd count to return
to its starting value. Failure stops the test; timeouts never count as passes.

The test records teardown time and `MemAvailable`. The timed interval includes
unmapping `kvm_run`, closing the vCPU/device/VM fds, and unmapping userspace
guest RAM; it is not an isolated stage-2-unmap measurement. These measurements do not
prove absence of kernel allocation or pinned-page leaks, and three 4 KiB
lifecycles do not validate large-memory teardown or SMP guests. Larger-memory
cases exercise the Core's eager pin/map and bulk teardown path, while still
executing only the tiny MMIO guest. No arbitrary teardown-time threshold is
used as a correctness criterion.

From the Linux checkout, with the repository's pinned Rust toolchain available:

```sh
python3 tools/axvisor/tests/arm64-shared/build-initramfs.py \
  --output /path/to/smoke-initramfs
python3 tools/axvisor/build-arm64.py \
  --output /path/to/host-build \
  --config tools/axvisor/tests/arm64-shared/kernel.config \
  --features arm-vhe,control
python3 tools/axvisor/tests/arm64-shared/run-qemu.py \
  --kernel /path/to/host-build/arch/arm64/boot/Image \
  --config /path/to/host-build/.config \
  --initramfs /path/to/smoke-initramfs/initramfs.cpio.gz \
  --output /path/to/new-result-directory --cpus 2
```

Repeat with `--cpus 4` and a new result directory after the two-CPU host passes.
The guest still has one vCPU. The host uses QEMU `virt`, GICv3, a homogeneous
`cortex-a76` CPU model, and EL2 (`virtualization=on`). The A76 model matches
the board's 40-bit PA / three-level stage-2 profile. QEMU `max` advertises a
larger PA range that the unchanged Core rejects without its four-level
page-table feature; it is not the default model for this board-targeted test.
In QEMU 8.2.2's `aarch64_a76_initfn`, `ID_AA64MMFR0_EL1=0x101122`
advertises PA range 2 (40 bits), and `ID_AA64MMFR1_EL1=0x10212122`
advertises VHE. See the [QEMU model source](https://github.com/qemu/qemu/blob/v8.2.2/target/arm/tcg/cpu64.c).
Do not use a Core path override
or bypass the adapter's migration guard to run this test. A guard rejection
means the adapter is not ready for execution.

The runner terminates QEMU on an explicit kernel/Core/test failure marker,
or on timeout, and records a failing verdict. A panic loop must not be left
running until a later successful-looking line appears.

To exercise memory teardown, create separate initramfs directories with
`build-initramfs.py --memory-mib 64|512|1024 --rounds 3`, and a further
`--memory-mib 64 --rounds 30` case. The builder's `--memory-mib` controls guest
RAM; zero means 4 KiB. The runner's independent `--memory-mib` controls QEMU
host RAM: use 1024 for the 64/512 MiB cases and 3072 for the 1024 MiB case.
Run the larger QEMU cases sequentially. The runner verifies the initramfs
hash against its adjacent manifest, reads the expected rounds and memory
size from that manifest, and checks their runtime declarations. An explicit
`--rounds` is available when no manifest exists; it must agree when one does.
Every round must balance its fds and emit one teardown-time measurement.

For an explicit CPU-pool test, build a separate image:

```sh
python3 tools/axvisor/tests/arm64-shared/build-initramfs.py \
  --output /path/to/pool-initramfs --guest-cpus 2-3 --outside-cpu 0
python3 tools/axvisor/tests/arm64-shared/run-qemu.py \
  --kernel /path/to/host-build/arch/arm64/boot/Image \
  --config /path/to/host-build/.config \
  --initramfs /path/to/pool-initramfs/initramfs.cpio.gz \
  --output /path/to/new-pool-result-directory --cpus 4
```

The runner derives `axvisor_linux.guest_cpus=2,3` from the image manifest.
The prefix is the composite driver's `KBUILD_MODNAME=\"axvisor_linux\"`, as
confirmed in the full kernel build's `.axvisor_arm64.o.cmd`, rather than the
individual C filename. Raw `sched_setaffinity` and `getcpu` calls verify
actual placement. Ordinary ioctls rotate between pool CPUs; VM creation and
KVM_RUN use different pool CPUs in each round, with coverage of every pool
CPU required. Outside the pool, both API-version and VM-creation ioctls must
return `EXDEV` without changing fd counts. Each round closes its VM/vCPU/
device fds while pinned outside the pool, checks balance, then re-enters the
pool for the next round. This tests movement between ioctl calls, not forced
CPU migration inside a migration-pinned ioctl.

The C program uses raw AArch64 syscalls, so no cross libc is required. Its UAPI
layouts are taken from the Linux v6.12 headers in this tree. The builder needs
Python 3 and `aarch64-linux-gnu-gcc`; the runner needs `qemu-system-aarch64`.
Build outputs and result directories belong outside tracked source paths.
