# Linux ARM SIMD handoff regression

This QEMU-only harness uses Linux 6.12's real `kernel_neon_begin/end`,
`TIF_KERNEL_FPSTATE`, IRQ state and scheduler. It extracts the production
`axvisor_linux_vhe_pause_simd/resume_simd` functions verbatim and compiles the
actual `rust/host-call.rs` RAII implementation with the adapter's AArch64
softfloat target. The test does not enable AxVisor or execute shared Core or a
VM, and does not establish guest world-switch correctness.

Six cases verify no-owner calls, nested Linux NEON on normal and error
returns, sleeping Linux callbacks, IRQ-disabled calls, and an actual task
switch through another SIMD user pinned to the same CPU. The last case
writes different values into `d0` in the two tasks and requires the outer
value to survive. Ownership assertions alone would not establish register
preservation.

Use a disposable Linux source worktree. The generator adds a test initcall
which powers QEMU off; never deploy this test kernel to a workstation or board.
Run these commands from the Linux adapter repository, with the cross compiler,
QEMU and the repository's `nightly-2026-05-28` Rust toolchain available:

```sh
FH_TESTS=$(realpath drivers/virt/axvisor/tests/vhe-simd)
FH_ADAPTER=$(realpath drivers/virt/axvisor)
git worktree add --detach ../linux-simd-test HEAD
FH_SRC=$(realpath ../linux-simd-test)
mkdir -p ../build-simd/lib ../simd-evidence
FH_BUILD=$(realpath ../build-simd)
FH_EVIDENCE=$(realpath ../simd-evidence)
python3 "$FH_TESTS/generate-test.py" --linux-source "$FH_SRC" \
  --adapter-source "$FH_ADAPTER" --output "$FH_EVIDENCE"
(
  cd "$FH_EVIDENCE/rust"
  cargo +nightly-2026-05-28 rustc --release --features arm-vhe \
    --target aarch64-linux-kernel.json -Zbuild-std=core,compiler_builtins \
    -Zjson-target-spec -- --emit=obj="$FH_BUILD/lib/fh_vhe_simd_rust.o"
)
make -C "$FH_SRC" O="$FH_BUILD" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
  KCONFIG_ALLCONFIG="$FH_TESTS/kernel.config" allnoconfig
make -C "$FH_SRC" O="$FH_BUILD" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j6 Image
python3 "$FH_TESTS/run-qemu.py" --image "$FH_BUILD/arch/arm64/boot/Image" \
  --output "$FH_EVIDENCE/positive"
```

Require all six case markers, `failures=0`, all five Rust-to-C callbacks,
normal QEMU exit and no kernel warning, BUG, panic or atomic-sleep warning.
Keep the generated provenance, final `.config`, compiler versions, Image
hash, build logs and full console log outside the repository.

For a negative control, first preserve the passing Image and evidence. Run
the generator into a **different evidence directory** with
`--negative-control`; this deliberately makes the extracted adapter handoff
inert while leaving Linux and the original Rust RAII code unchanged. Rebuild
Image and run `run-qemu.py --expect-negative-control`. It must detect failures
in all three nested-callback cases. The test callback checks ownership before
calling Linux's nesting `BUG_ON`, so that expected failure can be recorded
without crashing the test kernel. Regenerate the positive test afterwards.
