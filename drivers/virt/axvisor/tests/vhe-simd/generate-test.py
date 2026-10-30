#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Install exact SIMD helpers and Rust host-call code in a disposable Linux tree."""
import argparse
import hashlib
import json
import re
from pathlib import Path


def extract_function(text, name):
    matches = list(re.finditer(rf"(?m)^(?:bool|void) {name}\(", text))
    if len(matches) != 1:
        raise ValueError("expected one function: " + name)
    start = matches[0].start()
    end = re.search(r"(?m)^}", text[start:])
    if end is None:
        raise ValueError("missing function end: " + name)
    return text[start:start + end.end()]


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--linux-source", type=Path, required=True)
    parser.add_argument("--adapter-source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--negative-control", action="store_true")
    args = parser.parse_args()
    linux, adapter, output = (p.resolve() for p in
                              (args.linux_source, args.adapter_source, args.output))
    if not (linux / "arch/arm64/kernel/fpsimd.c").is_file():
        parser.error("expected a disposable ARM64 Linux source tree")
    fpsimd = (linux / "arch/arm64/kernel/fpsimd.c").read_bytes()
    if b"TIF_KERNEL_FPSTATE" not in fpsimd:
        parser.error("Linux lacks schedulable kernel FPSIMD ownership")
    raw_c = (adapter / "axvisor_arm64.c").read_bytes()
    raw_rust = (adapter / "rust/host-call.rs").read_bytes()
    target = (adapter / "rust/targets/aarch64-linux-kernel.json").read_bytes()
    target_data = json.loads(target)
    if target_data.get("abi") != "softfloat" or "-neon" not in target_data.get("features", ""):
        parser.error("test must use the actual softfloat ARM64 adapter target")
    functions = "\n\n".join(extract_function(raw_c.decode(), name) for name in
                             ("axvisor_linux_vhe_pause_simd", "axvisor_linux_vhe_resume_simd")) + "\n"
    tests = Path(__file__).parent
    installed = functions
    if args.negative_control:
        # Deliberately omit the adapter ownership handoff. Linux's actual
        # SIMD and scheduler implementations are unchanged. The callback
        # reports nesting before Linux's BUG_ON so the full suite can finish.
        installed = ("bool axvisor_linux_vhe_pause_simd(void) { return false; }\n"
                     "void axvisor_linux_vhe_resume_simd(bool paused) { (void)paused; }\n")
    generated = (tests / "fh_vhe_simd_test.c.in").read_text().replace(
        "/* EXTRACTED_SIMD_HELPERS */", installed)
    output.mkdir(parents=True, exist_ok=True)
    rust = output / "rust"
    rust.mkdir(exist_ok=True)
    (rust / "host-call.rs").write_bytes(raw_rust)
    (rust / "shim.rs").write_bytes((tests / "shim.rs").read_bytes())
    (rust / "aarch64-linux-kernel.json").write_bytes(target)
    (rust / "Cargo.toml").write_text('''[package]
name = "fh-vhe-simd-test"
version = "0.1.0"
edition = "2024"
[lib]
path = "shim.rs"
crate-type = ["rlib"]
[features]
arm-vhe = []
[profile.release]
panic = "abort"
codegen-units = 1
''')
    (output / "helpers-exact.c").write_text(functions)
    (linux / "lib/fh_vhe_simd_test.c").write_text(generated)
    makefile = linux / "lib/Makefile"
    line = "obj-y += fh_vhe_simd_test.o fh_vhe_simd_rust.o"
    current = makefile.read_text()
    if line not in current.splitlines():
        makefile.write_text(current.rstrip() + "\n\n" + line + "\n")
    provenance = {
        "negative_control": args.negative_control,
        "adapter_source_sha256": digest(raw_c),
        "helpers_sha256": digest(functions.encode()),
        "host_call_sha256": digest(raw_rust),
        "target_sha256": digest(target),
        "linux_fpsimd_sha256": digest(fpsimd),
        "shim_sha256": digest((rust / "shim.rs").read_bytes()),
        "generated_test_sha256": digest(generated.encode()),
        "scope": "real Linux FPSIMD ownership and Rust RAII regression; no Core or guest execution",
    }
    (output / "source-provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
    print(json.dumps(provenance, indent=2))


if __name__ == "__main__":
    main()
