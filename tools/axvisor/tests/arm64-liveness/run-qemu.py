#!/usr/bin/env python3
"""Check that a same-CPU sender can interrupt a non-cooperating guest."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", required=True, type=Path)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--assets", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--timeout", type=int, default=30)
    args = parser.parse_args()
    if args.timeout < 1:
        parser.error("timeout must be positive")
    config = args.config.read_text().splitlines()
    for required in ("CONFIG_AXVISOR_ARM64_VHE=y", "CONFIG_AXVISOR_LINUX_CONTROL=y"):
        if required not in config:
            parser.error("hosted VHE control configuration required")
    if "CONFIG_KVM=y" in config or "CONFIG_KVM=m" in config:
        parser.error("native KVM is not a hosted Core test")
    manifest = json.loads((args.assets / "manifest.json").read_text())
    image = args.assets / "initramfs.cpio.gz"
    if sha(image) != manifest["initramfs_sha256"]:
        parser.error("initramfs hash mismatch")
    cpu = manifest["cpu"]
    if not 0 <= cpu < 4:
        parser.error("test CPU must be online in this four-CPU host")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    command = ["qemu-system-aarch64", "-machine", "virt,virtualization=on,gic-version=3",
               "-cpu", "cortex-a76", "-accel", "tcg,thread=multi", "-smp", "4", "-m", "1024",
               "-display", "none", "-monitor", "none", "-serial", "stdio", "-no-reboot",
               "-kernel", str(args.kernel.resolve()), "-initrd", str(image.resolve()),
               "-append", "console=ttyAMA0 rdinit=/init panic=-1 axvisor_linux.control=1 "
                          f"axvisor_linux.guest_cpus={cpu}"]
    metadata = {"command": command, "manifest": manifest,
                "kernel_sha256": sha(args.kernel), "config_sha256": sha(args.config),
                "runner_sha256": sha(Path(__file__)), "scope": "functional, not a latency benchmark"}
    (out / "run.json").write_text(json.dumps(metadata, indent=2) + "\n")
    (out / "host.config").write_bytes(args.config.read_bytes())
    started = time.monotonic()
    timed_out = False
    with (out / "console.log").open("wb") as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        try:
            result = process.wait(timeout=args.timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            process.kill()
            result = process.wait()
    text = (out / "console.log").read_text(errors="replace")
    faults = re.findall(r"ARM64_LIVENESS_(?:FAIL|RESULT=FAIL)|Kernel panic|core panic:|"
                        r"BUG:|WARNING:|Oops:|Call trace:|soft lockup|rcu[^\n]*stall", text, re.I)
    checks = {"natural_zero_exit": result == 0 and not timed_out,
              "hosted_endpoint": "AxVisor KVM control endpoint registered: kvm" in text,
              "result": "ARM64_LIVENESS_RESULT=PASS" in text,
              "signal": text.count("SAME_CPU_SIGNAL_EINTR=PASS") == manifest["rounds"],
              "rounds": re.findall(r"^ROUND_PASS=(\d+)\r?$", text, re.M)
                        == [str(i) for i in range(1, manifest["rounds"] + 1)],
              "fd_balance": "\nFD_BALANCE=PASS" in text, "no_faults": not faults}
    verdict = {"pass": all(checks.values()), "checks": checks, "faults": faults,
               "timed_out": timed_out, "returncode": result,
               "elapsed_seconds": time.monotonic() - started}
    (out / "verdict.json").write_text(json.dumps(verdict, indent=2) + "\n")
    print(json.dumps(verdict, indent=2))
    raise SystemExit(0 if verdict["pass"] else 1)


if __name__ == "__main__":
    main()
