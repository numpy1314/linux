# Linux adapter heap regression

These tests exercise the heap callbacks extracted from the supplied
`axvisor_runtime.c` inside an actual Linux kernel. There are no allocation API
stubs. They are scoped to the ordinary heap allocation failure observed while
registering a 2 GiB ARM guest, and the matching free paths. They do not claim a
complete Firecracker boot, workload run, or VM teardown pass.

The reported failing request was 4,194,320 bytes: 524,288 page addresses times
8 bytes, plus a 16-byte Arc header. With 4 KiB pages and `MAX_PAGE_ORDER=10`, the
old rounded-size-plus-`kmalloc` path exceeds the 4 MiB allocation limit. A
separate legacy boot tests that old expression and expects its allocator
warning and NULL return; it intentionally does not enter the Rust panic loop.

## Inputs and tools

Use Python 3, GNU make, a Linux-capable C compiler and binutils, the usual Linux
build dependencies, GNU `timeout`, and QEMU system emulation. ARM64 cross-builds
use `aarch64-linux-gnu-`; x86_64 can use a native x86_64 compiler. No userspace
root filesystem or initramfs is needed: the built-in late initcall runs the
tests and powers off the emulated machine.

Use a **disposable clean Linux 6.12 source tree**. The generator installs
`lib/fh_heap_test.c` and adds its object to `lib/Makefile`. The adapter runtime
source can live in a different checkout and is only read. The generated source
contains its exact allocator/deallocator text, with SHA256 provenance recorded
in the output directory. No patches to the shared hypervisor Core are needed.

Official upstream source is available at
https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.12.tar.xz
(SHA256 `b1a2562be56e42afb3f8489d4c2a7ac472ac23098f1ef1c1e40da601f54625eb`).

## ARM64 reproduction

Set paths for this test directory, the disposable Linux tree, the candidate
adapter runtime, the **original board kernel configuration**, and a new build
directory. These are example relative paths; resolve them before calling make.

```sh
FH_TESTS=$(realpath drivers/virt/axvisor/tests/heap)
FH_SRC=$(realpath ../linux-6.12-test)
FH_RUNTIME=$(realpath drivers/virt/axvisor/axvisor_runtime.c)
FH_CONFIG=$(realpath ../rk3588-board.config)
mkdir -p ../heap-test-build
FH_OUT=$(realpath ../heap-test-build)

python3 "$FH_TESTS/generate-test.py" --linux-source "$FH_SRC" \
  --runtime-source "$FH_RUNTIME" --output "$FH_OUT/evidence"
cp "$FH_CONFIG" "$FH_OUT/.config"
"$FH_SRC/scripts/config" --file "$FH_OUT/.config" \
  --enable SERIAL_AMBA_PL011 --enable SERIAL_AMBA_PL011_CONSOLE
make -C "$FH_SRC" O="$FH_OUT" ARCH=arm64 \
  CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
make -C "$FH_SRC" O="$FH_OUT" ARCH=arm64 \
  CROSS_COMPILE=aarch64-linux-gnu- -j8 Image
python3 "$FH_TESTS/run-qemu.py" --arch arm64 \
  --image "$FH_OUT/arch/arm64/boot/Image" --output "$FH_OUT/evidence"
```

Required board-matched settings are 4 KiB pages, `CONFIG_ARCH_FORCE_MAX_ORDER=10`,
`CONFIG_ARM64_VA_BITS=39`, `CONFIG_PREEMPT_VOLUNTARY=y`,
`CONFIG_PREEMPT_DYNAMIC=n`, `CONFIG_NR_CPUS=8`, `CONFIG_SMP=y`,
`CONFIG_CMA=y`, and `CONFIG_ARM64_PSEUDO_NMI=y`. Preserve the uploaded board
configuration rather than substituting a small-memory smoke-test config.
Retain both input and final `.config`, plus `scripts/diffconfig` output, as
evidence. Vanilla Linux drops the custom AXVISOR config symbols absent from it;
the allocator itself is linked through the generated harness.

QEMU uses `virt`, GICv3, eight Cortex-A76 CPUs, 8 GiB RAM, TCG, and `cma=128M`.
The test task runs on CPU 4. This reproduces the relevant allocation boundary,
not RK3588 peripherals or its heterogeneous four-A55/four-A76 topology. The
command line deliberately enables GIC priority masking to check the raw
DAIF.I case that Linux `irqs_disabled()` alone can miss.

## Additional preemption-disabled coverage

The board config has `CONFIG_PREEMPT_COUNT=n`, so `preempt_disable()` by itself
does not create a count detectable by `in_atomic()`. The base test explicitly
reports this case as skipped instead of claiming it was exercised.

Build a second variant in another output directory, retaining voluntary
preemption but enabling these debug checks before `olddefconfig`:

```sh
"$FH_SRC/scripts/config" --file "$FH_OUT/.config" \
  --enable DEBUG_KERNEL --enable DEBUG_ATOMIC_SLEEP
```

`DEBUG_ATOMIC_SLEEP` selects `PREEMPT_COUNT`. Rebuild and run with
`run-qemu.py --preempt-count` (and the same other arguments). This verifies that
IRQ-enabled process context with preemption disabled takes the non-sleeping
allocation path, rejects an oversized request, and frees preallocated vmalloc
storage without atomic-sleep warnings. This is an explicitly different debug
configuration, not a claim that the board enables these options.

## x86_64 regression

The same generator works with a disposable x86_64 Linux 6.12 tree. Start from
`x86_64_defconfig`, configure voluntary preemption with dynamic preemption off,
8 CPUs, 4 KiB pages, maximum page order 10, serial console and ACPI poweroff,
then build `bzImage`. Use `ARCH=x86_64` and a native compiler or an appropriate
cross prefix. Run:

```sh
python3 "$FH_TESTS/run-qemu.py" --arch x86_64 \
  --image "$FH_OUT/arch/x86/boot/bzImage" --output "$FH_OUT/evidence"
```

The raw ARM DAIF case is explicitly skipped. The preemption-count debug variant
works as described above. This guards the common Linux allocator; it is not a
repeat of the historical x86 Firecracker benchmark.

## Assertions and acceptance

Covered cases include:

- Exact 4 MiB + 16-byte failure boundary and 8 MiB allocation.
- Zero-size and small allocations, 4 KiB and 64 KiB alignment, and a one-byte
  allocation with 64 KiB alignment.
- Every byte written and read with a page-varying pattern.
- Invalid alignment and size/alignment/page-rounding overflow rejection.
- The 524,288-entry pinned-page array, plus 524,289 entries (4 MiB + 8 bytes)
  forced onto vmalloc, zero initialization, data integrity, and both normal
  and IRQ-disabled frees.
- Small atomic allocations, oversized atomic rejection, IRQ-disabled free,
  ARM raw-DAIF free, and optional IRQ-enabled/preemption-disabled context.

Expected fixed assertions are ARM64 55 (60 with preemption-count coverage), or
x86_64 52 (57 with that coverage). The legacy test has 3 assertions. Acceptance
requires the expected final marker, zero failures, and no WARNING, BUG, Oops,
panic, RCU stall, or atomic-sleep warning in the **fixed** log. ARM64 also must
show `RAW_DAIF linux_irqs_disabled=0 bridge_atomic=1`. `verify-results.py`
enforces these conditions; QEMU exit status alone is insufficient.

Deferred frees are allowed time to run, and a subsequent large allocation is
checked. Returning from a free and seeing no warning does not prove every
possible lifetime/interleaving, leak freedom, NMI safety, or full VM shutdown.
Keep logs, final configs, source provenance, compiler/QEMU versions, and the
tested source revision with any reported result.

## Recorded validation (2026-10-17)

Candidate runtime SHA256: `1f1af45408e1ab621487a9b27edfedf6dc275270552950016db594fa5f4f962b`.
Allocator block SHA256: `b38250cac7817986a26b0f663972aeb417cc4629e767afdba22cda8b2308a909`.

- ARM64, board-matched allocation settings: 55 checks passed. The old path
  reproduced the allocation-order rejection. Raw DAIF/PMR detection and forced
  pinned-array vmalloc were exercised; preempt-count-only coverage was explicitly
  skipped because the board configuration does not enable PREEMPT_COUNT.
- x86_64, Linux 6.12 tinyconfig plus SMP8, voluntary preemption and
  DEBUG_ATOMIC_SLEEP: 57 checks passed, including the preemption-disabled,
  IRQ-enabled process-context path. ARM-specific DAIF checks are inapplicable.
- Both fixed boots contained no WARNING/BUG/Oops/panic/RCU-stall markers. The
  complete candidate runtime C file also compiled with CONTROL enabled for
  both architectures (ARM64 additionally enabled VHE).
- A further ARM64 boot used the same Image without forcing
  `irqchip.gicv3_pseudo_nmi=1`, matching the absence of that option in the
  board's command line. All 55 assertions passed with no kernel faults;
  Linux's IRQ predicate observed DAIF masking in this mode. The forced
  priority-masking run remains the additional boundary test described above.
- Tests used GCC 13.3 and QEMU 8.2.2 TCG with 8 GiB RAM. They do not execute
  the complete hypervisor or reproduce the historical x86 benchmark. Actual
  board deployment and full VM lifecycle success remain separate gates.

The assertion counts comprise 52 common checks, plus 3 ARM-only DAIF checks
or 5 x86 debug/preemption-count checks. These are assertions, not counts of
independent VMs. The ARM board config leaves the preemption-count-only case
explicitly skipped; the x86 architecture cannot execute ARM DAIF checks.
