#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Boot and verify the real Linux/Rust SIMD boundary regression in QEMU."""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path

CASES = {"no_owner", "nested_success", "nested_error", "callback_sleep", "irqoff", "schedule"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--expect-negative-control", action="store_true")
    parser.add_argument("--timeout", type=int, default=90)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    command = ["qemu-system-aarch64", "-machine", "virt,gic-version=3", "-accel", "tcg",
               "-cpu", "cortex-a76", "-smp", "2", "-m", "512M", "-nographic",
               "-monitor", "none", "-no-reboot", "-kernel", str(args.image.resolve()),
               "-append", "console=ttyAMA0 loglevel=7 panic=-1"]
    with (args.output / "console.log").open("w") as log:
        run = subprocess.run(["timeout", "--kill-after=5s", f"{args.timeout}s", *command],
                             stdout=log, stderr=subprocess.STDOUT)
    log = (args.output / "console.log").read_text(errors="replace")
    case_entries = re.findall(r"FH_SIMD CASE (\w+) failures=(\d+)", log)
    cases = {name: int(count) for name, count in case_entries}
    results = re.findall(r"FH_SIMD RESULT checks=(\d+) failures=(\d+) callbacks=(\d+) completed=1", log)
    faults = re.findall(r"(?mi)^.*(?:BUG:|WARNING:|Oops:|Call trace:|Kernel panic|"
                        r"sleeping function called from invalid context|rcu.*stall).*$", log)
    completed = len(results) == 1
    counts = dict(zip(("checks", "failures", "callbacks"), map(int, results[0]))) if completed else {}
    accepted = (run.returncode == 0 and completed and set(cases) == CASES and
                len(case_entries) == len(CASES) and counts.get("callbacks") == 5 and not faults)
    if args.expect_negative_control:
        accepted &= (counts.get("failures", 0) > 0 and
                     cases.get("nested_success", 0) > 0 and
                     cases.get("nested_error", 0) > 0 and cases.get("callback_sleep", 0) > 0)
    else:
        accepted &= counts.get("failures") == 0 and counts.get("callbacks") == 5 and all(
            value == 0 for value in cases.values())
    record = {"accepted": bool(accepted), "negative_control": args.expect_negative_control,
              "qemu_returncode": run.returncode, "cases": cases, "counts": counts,
              "kernel_faults": faults, "command": command,
              "image_sha256": hashlib.sha256(args.image.read_bytes()).hexdigest(),
              "qemu_version": subprocess.check_output(["qemu-system-aarch64", "--version"], text=True).splitlines()[0],
              "scope": "Linux FPSIMD ownership, callback nesting and task switch; no VM or shared Core"}
    (args.output / "result.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps(record, indent=2))
    return 0 if accepted else 1


if __name__ == "__main__":
    raise SystemExit(main())
