# Linux adapter eventfd regression

This harness exercises the exact retained-fd get/read/write/release callbacks
and task completion/join functions extracted from `axvisor_runtime.c` inside
a real Linux 6.12 kernel. It does not substitute mock eventfd, poll, scheduler,
wait queue or reference-count implementations. It does not execute a VM and
does not establish ARM VHE guest-entry or VM-teardown correctness.

Linux 6.12 has no public `eventfd_file_create()` helper. Its real `do_eventfd()`
is private to `fs/eventfd.c`, so the generator appends one include to that file
in a **disposable source tree**. The included test can call `do_eventfd()` and
inspect the real eventfd wait queue under its lock. It creates ordinary
nonblocking eventfds in the init task's fd table and retains them through the
adapter callback. No production kernel or shared Core source is changed.

The generated late initcall powers the emulated machine off when done. Run
this kernel only in QEMU, never as a normal workstation or board kernel.
Do not combine it with another test initcall which powers off first.

## Generate and build

The adapter runtime may be in another checkout. `--linux-source` must name a
disposable Linux 6.12 source tree with the AxVisor bridge disabled. Provenance
includes runtime, extracted callback and generated-source SHA256 digests.

```sh
FH_TESTS=$(realpath drivers/virt/axvisor/tests/eventfd)
FH_SRC=$(realpath ../linux-6.12-eventfd)
FH_RUNTIME=$(realpath drivers/virt/axvisor/axvisor_runtime.c)
mkdir -p ../eventfd-arm64-build
FH_OUT=$(realpath ../eventfd-arm64-build)

python3 "$FH_TESTS/generate-test.py" --linux-source "$FH_SRC" \
  --runtime-source "$FH_RUNTIME" --output "$FH_OUT/evidence"
make -C "$FH_SRC" O="$FH_OUT" ARCH=arm64 \
  CROSS_COMPILE=aarch64-linux-gnu- allnoconfig
"$FH_SRC/scripts/config" --file "$FH_OUT/.config" \
  --enable EXPERT --enable PRINTK --enable SMP --set-val NR_CPUS 2 \
  --enable ARM_PSCI_FW --enable EVENTFD --enable TTY \
  --enable SERIAL_AMBA_PL011 --enable SERIAL_AMBA_PL011_CONSOLE \
  --enable DEBUG_KERNEL --enable DEBUG_ATOMIC_SLEEP \
  --disable AXVISOR_LINUX_BRIDGE
make -C "$FH_SRC" O="$FH_OUT" ARCH=arm64 \
  CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
make -C "$FH_SRC" O="$FH_OUT" ARCH=arm64 \
  CROSS_COMPILE=aarch64-linux-gnu- -j8 Image
timeout 90s qemu-system-aarch64 -machine virt,gic-version=3 \
  -cpu cortex-a76 -smp 2 -m 512M -nographic -no-reboot \
  -kernel "$FH_OUT/arch/arm64/boot/Image" \
  -append 'console=ttyAMA0 loglevel=7 panic=-1' \
  > "$FH_OUT/evidence/console.log" 2>&1
```

For x86_64, use a separate build directory and native compiler. Start from
`ARCH=x86_64 allnoconfig`, then enable `64BIT`, `EXPERT`, `PRINTK`, `SMP`,
`EVENTFD`, `TTY`, `SERIAL_8250`, `SERIAL_8250_CONSOLE`, `ACPI`, `PCI`, `PM`,
`DEBUG_KERNEL` and `DEBUG_ATOMIC_SLEEP`, and set `NR_CPUS=2`. Run
`olddefconfig` and build `bzImage`. Run
`qemu-system-x86_64 -machine q35 -cpu max -smp 2 -m 2G
-nographic -no-reboot -kernel .../arch/x86/boot/bzImage -append
'console=ttyS0 loglevel=7 panic=-1'`. No userspace rootfs/initramfs is needed.

Use 2 GiB for x86 when testing this repository's enlarged 2 MiB kernel stacks
(`THREAD_SIZE_ORDER=9`). Linux derives its thread limit from available RAM
and stack size; at 512 MiB, boot can exhaust that small limit before this
test's first reader is created, producing `kthread_create errno=-11`.
That setup failure does not exercise the eventfd reader.

## Required results

All five case markers must report zero failures:

- `queued_before_reader`: an event already present before reader startup is
  consumed without waiting for a future event.
- `idle_then_wake`: with no event, a real poll waiter is installed and the
  reader remains `TASK_INTERRUPTIBLE`; over 150 ms its scheduler CPU time is
  less than one eighth of elapsed time plus 1 ms. Signalling one event resumes
  the read and returns the counter value. This is a functional busy-loop
  check, not a latency benchmark.
- `cancel_write_join_release`: mimic Core's ordering: publish cancellation,
  adapter write of one, actual adapter join, then retained reference release.
  The cancellation event must not be counted as an injected interrupt.
- `signal_interrupt`: permit and send SIGUSR1 to a blocked reader, require
  `-EINTR`, and verify poll waiter removal.
- `repeated_lifetimes`: 32 independent eventfd/reader lifetimes, alternating
  a verified sleeping waiter and an immediate wake racing with registration.
  Check the result, task exit, empty wait queue, original fd closure and that
  only the independent observer's context reference remains after release.

Acceptance requires `FH_EVENTFD RESULT checks=... failures=0 completed=1`,
all five case markers, no `FAIL`/`ABORT`, and no kernel WARNING, BUG, Oops,
panic, RCU stall or atomic-sleep warning. QEMU exit status alone is not enough.
The 5-second completion deadline is a failure limit, not a timing assertion.
Reference checks cover tested objects and interleavings; they are not a proof
of general leak freedom or full VM cleanup.

Keep the final config, full console log, source provenance, compiler/QEMU
versions and candidate commit with results. The generator also accepts the
previous read implementation for a negative-control boot: the idle/blocking
assertions must fail, demonstrating that the test detects the regression.
