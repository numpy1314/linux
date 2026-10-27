#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Run real-kernel irqfd component checks and reject kernel faults/timeouts."""
import argparse
import re
import subprocess
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", required=True, choices=("arm64", "x86_64"))
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--qemu-data", type=Path)
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()
    if args.arch == "arm64":
        command = ["qemu-system-aarch64", "-machine", "virt,gic-version=3",
                   "-cpu", "cortex-a76"]
        cmdline = "console=ttyAMA0 earlycon=pl011,0x9000000"
        expected = 997
        memory = "512"
    else:
        command = ["qemu-system-x86_64", "-machine", "q35", "-cpu", "max"]
        cmdline = "console=ttyS0 earlyprintk=serial"
        expected = 10
        memory = "2048"
    command += ["-accel", "tcg,thread=multi", "-smp", "4", "-m", memory,
                "-nodefaults", "-nographic", "-monitor", "none", "-serial", "stdio",
                "-no-reboot", "-kernel", str(args.image.resolve()),
                "-append", cmdline + " loglevel=7 panic=-1"]
    if args.qemu_data:
        command += ["-L", str(args.qemu_data.resolve())]
    args.output.mkdir(parents=True, exist_ok=True)
    log_path = args.output / "qemu.log"
    with log_path.open("w") as log:
        try:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                    timeout=args.timeout)
        except subprocess.TimeoutExpired:
            print("FAIL: QEMU timed out; inspect", log_path)
            return 1
    text = log_path.read_text(errors="replace")
    summary = re.findall(r"FH_IRQFD DONE checks=(\d+) failures=(\d+)", text)
    faults = re.search(r"FH_IRQFD FAIL|WARNING:|BUG:|Oops:|Kernel panic|"
                       r"sleeping function called|possible circular locking|"
                       r"inconsistent lock state|rcu.*stall", text, re.I)
    if result.returncode or summary != [(str(expected), "0")] or faults:
        print("FAIL:", "qemu_rc=", result.returncode, "summary=", summary,
              "fault=", faults.group(0) if faults else None, "log=", log_path)
        return 1
    print(f"PASS: {args.arch}, {expected} assertions, no kernel fault markers")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
