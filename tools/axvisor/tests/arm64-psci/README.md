# ARM64 PSCI/SMCCC microguest checks

This fixture creates one VM and two vCPUs using actual KVM ioctls. It executes
real HVC/SMC instructions and reuses the same VM across the cases. It is a
functional check; QEMU elapsed times are not performance measurements.

It covers:

- HVC and SMC `SYSTEM_OFF` / `SYSTEM_RESET`, with exact `KVM_EXIT_SYSTEM_EVENT`,
  shutdown/reset type, `ndata=1`, and zero flags in `data[0]`.
- An unsupported PSCI function through HVC and SMC; an unsupported non-PSCI SMC;
  and `SMC #1` for an otherwise supported reset function. These must return -1,
  advance the PC correctly, and reach the following MMIO store.
- A private HVC with the same function ID as the unsupported SMC, preserving
  the existing private HVC/unknown-exit path rather than swallowing it as SMC.
- `CPU_ON` for an already runnable target, returning `ALREADY_ON` without
  overwriting the target PC or context register.
- A successful `CPU_ON` of a stopped target, followed by `GET_ONE_REG` before
  its first run. The read must observe the reset PC and context; a subsequent
  `SET_ONE_REG` override must survive the first run rather than being replaced
  by a late reset.
- All four system-event combinations while the other vCPU runs a tight shared
  counter loop with guest IRQs masked. Once the originating `KVM_RUN` returns,
  both states must be STOPPED and the counter must stay unchanged. The secondary
  run must remain blocked until a host signal yields EINTR.
- A repeated run of the stopped originating vCPU blocks until a signal; after
  an explicit userspace `SET_MP_STATE(RUNNABLE)`, the guest resumes after the
  PSCI instruction with `INTERNAL_FAILURE` (-6) in x0.
- Full FD balance after the worker tasks and the one VM are destroyed.

Workers use `clone(CLONE_VM | SIGCHLD)` with separate stacks, so they retain the
creator's mm as native KVM requires. Parent and worker are placed on host CPU2
and CPU3 respectively. The runner uses a homogeneous four-CPU Cortex-A76 host
and admits CPU2-3. It defaults to `axvisor_linux.watchdog=0`, so the synchronizing
stop checks cannot depend on the adapter's periodic 5 ms timer. This requires
the corresponding adapter knob; `--watchdog on` provides a separate default
policy regression. The guest virtual timer PPI is not disabled by this knob.

```sh
python3 tools/axvisor/tests/arm64-psci/build-initramfs.py --output /tmp/psci-assets
python3 tools/axvisor/tests/arm64-psci/run-qemu.py \
  --kernel /absolute/path/to/Image --config /absolute/path/to/.config \
  --assets /tmp/psci-assets --output /tmp/psci-results
```

Only use this with an admitted, patched candidate in isolated QEMU. The old
Core may forward trapped SMC to its physical firmware. No host disk, board
boot medium, or real board is attached. A clean outer QEMU exit is required;
timeouts, externally terminated runs, test failures, panics and warnings fail
the verdict. Run directories must be new. Build and result directories belong
outside the source tree.

The runner also accepts `--backend kvm` with a native-KVM host configuration;
the private-HVC unknown-exit case is a Tessera frontend contract and may differ
from native KVM, so a native comparison needs that case accounted for explicitly.
