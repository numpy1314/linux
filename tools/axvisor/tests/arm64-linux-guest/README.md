# AArch64 Firecracker / Linux guest functional smoke

This test boots a real Firecracker v1.10.1 microVM on the hosted Core endpoint
inside an AArch64 QEMU host. The guest has one vCPU, 512 MiB RAM, Linux 5.10.223
and a 64 MiB ext4 root filesystem. `--vcpus 2` or `--vcpus 4` builds an SMP
guest. The guest checks its CPU mask and runs a timer wait on each guest CPU,
including the PSCI-started secondary CPUs. It is a functional check, not a benchmark.

Each guest run checks deterministic integer CPU work, fifty one-millisecond
`clock_nanosleep` calls, and 4 MiB of patterned file writes followed by `fsync`,
close, reopen and byte-for-byte read verification. It then requests a restart
or poweroff. Host PID 1 waits for Firecracker to exit with status zero before
launching the next independent VM. The default is three VM lifecycles in one
host boot. The root disk is reused across these lifecycles; each guest truncates
and rewrites the test file. A fresh outer QEMU run reconstructs the disk from
the initramfs. No host disk or board boot media is attached.

Dependencies are Python 3, `aarch64-linux-gnu-gcc`, `mke2fs`, and
`qemu-system-aarch64`. Download the two pinned upstream inputs into an asset
directory (the builder checks their full SHA-256 digests before use):

```sh
mkdir -p /tmp/fh-linux-inputs
curl -fL --retry 3 \
  https://github.com/firecracker-microvm/firecracker/releases/download/v1.10.1/firecracker-v1.10.1-aarch64.tgz \
  -o /tmp/fh-linux-inputs/firecracker-v1.10.1-aarch64.tgz
curl -fL --retry 3 \
  https://s3.amazonaws.com/spec.ccfc.min/firecracker-ci/v1.10/aarch64/vmlinux-5.10.223 \
  -o /tmp/fh-linux-inputs/Image-5.10.223
```

| Input | SHA-256 |
| --- | --- |
| Firecracker release archive | `9e3641071de140979afaac0c52fdc107baeba398bdb5709c12f77ee469207fcd` |
| Extracted Firecracker binary | `ff62dd7a90db03a056e61e94058b9439f47182809926545a5b555879f7816506` |
| Linux guest image | `eb5d95ac8a67f7a86acf0cb35625633713ad5170b56de8617808d0e18bb832ec` |

From the Linux source tree, build the test assets and run against the actual
host kernel and its matching configuration:

```sh
python3 tools/axvisor/tests/arm64-linux-guest/build-assets.py \
  --assets /tmp/fh-linux-inputs --output /tmp/fh-linux-restart --rounds 3
python3 tools/axvisor/tests/arm64-linux-guest/run-qemu.py \
  --kernel /absolute/path/to/host/arch/arm64/boot/Image \
  --config /absolute/path/to/host/.config \
  --assets /tmp/fh-linux-restart --output /tmp/fh-linux-restart-results \
  --timeout 180
```

The default guest shutdown operation is the Linux `RESTART` reboot command.
To test ordinary poweroff separately, build another asset directory with
`--guest-shutdown poweroff`, then run it with a different results directory.
A successful poweroff run does not establish that restart works.
For a quick check, use `--rounds 1 --guest-shutdown poweroff`; one 1-vCPU case
and one 2-vCPU case provide separate UP/SMP coverage without a soak test.

Shared Core revision `4152c3e780feb5e44241fb3fa143fe04bdca0181` does not
implement HVC PSCI `SYSTEM_RESET` as a VM exit. Its ARM exception layer emits
a `Hypercall`, but the Core KVM run loop consumes that call and returns
`NOT_SUPPORTED` to the guest. Existing `HostIf` and `Control` interfaces expose
no callback carrying the hypercall number, so this cannot be repaired through
the current Linux adapter API alone. Linux consequently reports `Reboot failed
-- System halted`. The runner stops this known failure promptly, records the
limitation in `verdict.json`, and still returns a failing status; this is never
included as a lifecycle PASS. Use one round for that diagnostic.

Source review also found that Core forwards unhandled SMC PSCI calls, including
`SYSTEM_RESET`, to physical EL3 firmware. This is a separate board-admission
blocker, not a tested feature. These assets are for isolated QEMU validation;
do not treat their poweroff success as permission to test untrusted guests on a
physical board. No SMC reset is issued by this fixture.

To exercise a four-CPU host with a guest execution pool restricted to host CPUs
2–3, create separately identified assets and results:

```sh
python3 tools/axvisor/tests/arm64-linux-guest/build-assets.py \
  --assets /tmp/fh-linux-inputs --output /tmp/fh-linux-pool23 \
  --rounds 3 --guest-shutdown poweroff --fc-cpus 2-3
python3 tools/axvisor/tests/arm64-linux-guest/run-qemu.py \
  --kernel /absolute/path/to/host/arch/arm64/boot/Image \
  --config /absolute/path/to/host/.config \
  --assets /tmp/fh-linux-pool23 --output /tmp/fh-linux-pool23-results \
  --cpus 4 --guest-cpus 2-3 --timeout 180
```

`--cpus` is the outer QEMU CPU count and `--guest-cpus` configures the adapter's
allowed host CPU pool; neither changes the microVM's single vCPU. The builder's
`--fc-cpus` compiles a host CPU affinity mask into the host init and records it
in the manifest. Before any KVM ioctl and before `exec`, each child sets and
reads back its affinity. Firecracker and the threads it creates inherit that
mask. The runner checks that the recorded affinity is a nonempty subset of the
admitted online pool and requires an exact matching readback in every round.
Without `--fc-cpus`, the process inherits the host init's allowed CPUs.

The host must use the admitted hosted VHE configuration: 4 KiB pages, bridge,
control and ARM64 VHE enabled, native KVM disabled, and CPU hotplug,
pseudo-NMI, SME, MTE and POE disabled. It needs executable ELF, devtmpfs,
procfs, sysfs, gzip initramfs support and Firecracker's futex/epoll/eventfd
facilities. The freestanding host init invokes Firecracker with `--no-api
--no-seccomp`, and no network device is configured. Host networking, ext4 and
seccomp are therefore unnecessary for this smoke. The guest itself supplies
ext4 and virtio-blk support. The runner deliberately uses Cortex-A76, a 40-bit
physical-address model matching the three-level stage-2 Core build, and
allocates 2 GiB to the outer QEMU host.

`manifest.json` records the build commands and artifact hashes. Results include
the full console log, an exact host configuration copy, run metadata with the
host Image/config/initramfs hashes, and `verdict.json`. PASS requires all guest
checks in every round, all Firecracker exits to be successful, all VM cleanup
completion markers, the host's final success marker and QEMU's natural zero
exit. Timeout, panic, RCU stall or other fault markers fail the test even when
some guest checks passed. A runner-initiated termination also fails even when
QEMU happens to return zero after SIGTERM. The guest's own PASS marker precedes
its shutdown request and therefore does not establish successful teardown;
the parent's `wait4` must observe a zero exit after Firecracker's threads and
file descriptors are released. Run output directories must be new so prior evidence
cannot be silently overwritten. Keep generated assets and results outside the
source tree.

The shared guest init initializes its I/O array so the ELF RW load segment is
file-backed. This avoids the unaligned, zero-file-size pure-BSS segment emitted
by the default bare static link, which Linux 5.10 rejects with `-EFAULT` before
running `/init`.
