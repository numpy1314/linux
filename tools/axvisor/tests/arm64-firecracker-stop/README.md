# Firecracker Finish while another guest vCPU is still busy

This tests the real Firecracker v1.10.1 exit/Finish/signal/join path. It avoids
PSCI SYSTEM_OFF/RESET, since those events already stop all vCPUs inside Core and
therefore cannot prove that Firecracker itself can interrupt a busy guest.

The freestanding AArch64 guest has a valid Linux Image header. CPU0 starts CPU1
with PSCI CPU_ON. CPU1 masks guest interrupts and continuously increments a
shared counter. CPU0 waits for at least 1,000 increments, prints the progress
marker via the emulated UART, and emits a non-PSCI private HVC. Core returns
KVM_EXIT_UNKNOWN for that HVC. Firecracker reports the intentionally unhandled
exit and takes its normal error-exit path, which must send Finish and interrupt
the other vCPU before joining and dropping its resources.

Firecracker exit code 1 is expected for this deliberately erroneous guest.
The host init must naturally reap that exact exit status, continue executing,
successfully query the KVM endpoint again, and preserve its fd count. The
runner also requires the busy-counter marker, the intended Unknown exit, Core
VM cleanup, no PSCI terminal exit, and a natural successful outer host exit.
A timeout, forced outer termination, kernel fault, missing cleanup, or any
other Firecracker status fails. Parent fd balance alone is not proof of zero
kernel leaks; it is paired with child reaping and the Core cleanup marker.

Both microVM vCPUs use 512 MiB RAM. There is no guest disk, network, or Linux
userspace. With no virtio devices or boot timer, the UART is the first MMIO
device and occupies Firecracker's first MMIO page at 0x40000000. That ordering
is fixed by the pinned v1.10.1 builder. No timer or cooperative halt is used by
the busy secondary guest. The host runner defaults to watchdog disabled, so
the adapter's periodic timer cannot hide a missing interruption mechanism.

Build one asset set per host CPU placement, then run each once in isolated QEMU:

```sh
python3 tools/axvisor/tests/arm64-firecracker-stop/build-assets.py \
  --assets /path/to/pinned-firecracker-inputs --output /tmp/fc-stop-pair --fc-cpus 2-3
python3 tools/axvisor/tests/arm64-firecracker-stop/run-qemu.py \
  --kernel /absolute/path/to/Image --config /absolute/path/to/.config \
  --assets /tmp/fc-stop-pair --output /tmp/fc-stop-pair-results
```

Repeat with a distinct build directory and `--fc-cpus 2` to exercise two vCPUs
sharing one host CPU. The outer host has four Cortex-A76 CPUs and admits CPU2-3.
Use new result directories. Build outputs and logs stay outside tracked source.
These are functional tests; their QEMU elapsed times are not performance data.
