# ARM Linux irqfd readiness waits

ARM VHE keeps the existing listener task and cancel/join/release lifecycle.
An empty eventfd is read with `IOCB_NOWAIT`, regardless of the shared file's
`O_NONBLOCK` flag. The Core then waits for readiness through the host API.
The host subscribes lazily to the real eventfd waitqueue; its callback only
wakes the listener. Counter consumption and guest IRQ injection remain in
task context. This removes the idle `WouldBlock`/yield loop for eventfds.

Cancellation is persistent and private to each retained reference. It wakes
the listener without adding a synthetic event to the shared counter. After
joining the listener, release detaches the callback under the eventfd queue
lock before dropping the file/context references. Readiness checks after
subscription and the waitqueue predicate's readiness recheck after enqueueing
cover events arriving around registration or the readiness-to-sleep boundary.

The new APIs and behavior are gated on ARM VHE. Other architectures retain
their existing read/yield and cancellation-write behavior. Non-eventfd
references return `Unsupported` from wait/cancel and retain the old fallback.
This is not native KVM's callback-to-interrupt fast path: it still incurs a
listener wakeup and does not change VGIC injection or vCPU kicks.

## Real-kernel component tests

Use a disposable Linux 6.12 source checkout, Python 3, normal Linux build
dependencies, QEMU system emulation, and an ARM64 cross compiler. The generator
extracts the exact fd-reference structure and callbacks from the supplied
runtime. It installs a built-in test and a test-only factory that creates real
eventfds through `do_eventfd()`. No eventfd, locking, or scheduler stubs are used.
The production source is only read; changes to `fs/eventfd.c` and `lib/Makefile`
belong only in the disposable tree.

Example, with paths relative to the adapter repository:

```sh
FH_TESTS=$(realpath drivers/virt/axvisor/tests/irqfd)
FH_RUNTIME=$(realpath drivers/virt/axvisor/axvisor_runtime.c)
FH_SRC=$(realpath ../linux-irqfd-test)
mkdir -p ../irqfd-arm-test
FH_OUT=$(realpath ../irqfd-arm-test)

python3 "$FH_TESTS/generate-test.py" --linux-source "$FH_SRC" \
  --runtime-source "$FH_RUNTIME" --output "$FH_OUT/evidence"
make -C "$FH_SRC" O="$FH_OUT" ARCH=arm64 \
  CROSS_COMPILE=aarch64-linux-gnu- tinyconfig
"$FH_SRC/scripts/config" --file "$FH_OUT/.config" \
  --enable PRINTK --enable TTY --enable SERIAL_AMBA_PL011 \
  --enable SERIAL_AMBA_PL011_CONSOLE --enable SMP --set-val NR_CPUS 4 \
  --enable EVENTFD --enable DEBUG_KERNEL --enable DEBUG_ATOMIC_SLEEP \
  --enable PROVE_LOCKING --enable ARM64_PSEUDO_NMI --enable EXPERT \
  --disable AXVISOR_LINUX_BRIDGE --disable KVM --enable ARM_PSCI_FW \
  --enable POWER_RESET --enable POWER_RESET_SYSCON
make -C "$FH_SRC" O="$FH_OUT" ARCH=arm64 \
  CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
make -C "$FH_SRC" O="$FH_OUT" ARCH=arm64 \
  CROSS_COMPILE=aarch64-linux-gnu- -j8 Image
python3 "$FH_TESTS/run-qemu.py" --arch arm64 \
  --image "$FH_OUT/arch/arm64/boot/Image" --output "$FH_OUT/evidence"
```

No root filesystem is required. The initcall runs the assertions and powers
off the emulated machine. The runner enforces the assertion count, zero
failures, no kernel fault markers, and completion before its timeout.

ARM coverage includes blocking and nonblocking eventfds, unchanged shared
file flags, exact counter reads, short-read rejection, a sleeping idle waiter,
cross-CPU notification, pre-existing readiness, cancellation before and during
waiting, semaphore counters, independent cancellation for two references to
one file, 128 registration/wakeup/cancel iterations, and 256 callback detach
iterations concurrent with an independent writer. The expected count is 997
assertions; the iterations are component stress, not VM lifecycle tests.
The ARM QEMU machine uses four CPUs and 512 MiB RAM.

For x86 regression, use a separate output directory, `ARCH=x86_64`, native GCC,
and `tinyconfig`. Enable `64BIT`, `PRINTK`, `TTY`, `SERIAL_8250`,
`SERIAL_8250_CONSOLE`, `SMP`, `EVENTFD`, `DEBUG_KERNEL`, `DEBUG_ATOMIC_SLEEP`,
`PROVE_LOCKING`, `EXPERT`, `PCI`, `ACPI`, and `ACPI_SLEEP`; set `NR_CPUS=4` and
disable `AXVISOR_LINUX_BRIDGE` and `KVM`. Build `bzImage` and run with
`--arch x86_64 --image "$FH_OUT/arch/x86/boot/bzImage"`. Its 10 assertions
exercise the unchanged read/write/reference path, including a ready blocking
eventfd. ARM-only wait/cancel operations are absent from this build. The x86
QEMU machine uses four CPUs and 2 GiB RAM.

Retain generated provenance, final configs, logs, and tested revisions with
test evidence outside the repository. Full hypervisor linking, board boot,
VM creation/destruction, and device I/O workloads are separate integration
checks. These component tests do not establish end-to-end IRQ latency or
native KVM equivalence.
