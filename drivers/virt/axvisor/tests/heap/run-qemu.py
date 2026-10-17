#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Boot isolated legacy/fixed allocator tests; neither boot needs a root filesystem."""
import argparse
import subprocess
import sys
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", required=True, choices=("arm64", "x86_64"))
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--preempt-count", action="store_true")
    parser.add_argument("--timeout", type=int, default=180)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    if args.arch == "arm64":
        machine = ["qemu-system-aarch64", "-machine", "virt,gic-version=3,virtualization=on",
                   "-cpu", "cortex-a76"]
        commandline = "console=ttyAMA0 earlycon=pl011,0x9000000 irqchip.gicv3_pseudo_nmi=1"
    else:
        machine = ["qemu-system-x86_64", "-machine", "q35", "-cpu", "max"]
        commandline = "console=ttyS0 earlyprintk=serial"
    common = machine + ["-accel", "tcg,thread=multi", "-smp", "8", "-m", "8192",
                        "-nodefaults", "-nographic", "-monitor", "none", "-serial", "stdio",
                        "-no-reboot", "-kernel", str(args.image.resolve())]
    for mode in ("legacy", "fixed"):
        command = common + ["-append", f"{commandline} loglevel=7 cma=128M fhtest={mode}"]
        with (args.output / f"qemu-{mode}.log").open("w") as log:
            result = subprocess.run(["timeout", "--kill-after=5s", f"{args.timeout}s", *command],
                                    stdout=log, stderr=subprocess.STDOUT)
        if result.returncode:
            print(f"{mode}: QEMU failed or timed out (rc={result.returncode}); inspect log", file=sys.stderr)
            return 1
    verify = [sys.executable, str(Path(__file__).with_name("verify-results.py")),
              "--legacy-log", str(args.output / "qemu-legacy.log"),
              "--fixed-log", str(args.output / "qemu-fixed.log"),
              "--arch", args.arch, "--output", str(args.output / "test-result.json")]
    if args.preempt_count:
        verify.append("--preempt-count")
    return subprocess.run(verify).returncode


if __name__ == "__main__":
    raise SystemExit(main())
