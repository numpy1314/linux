#!/usr/bin/env python3
"""Run the actual PSCI/SMCCC ioctl fixture in isolated four-CPU QEMU."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time

FAULTS = r"ARM64_PSCI_(?:FAIL|RESULT=FAIL)|HOST_POWEROFF_ERROR|Kernel panic|core panic:|panicked at|BUG:|WARNING:|Oops:|Call trace:|soft lockup|rcu[^\n]*stall"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", required=True, type=Path)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--assets", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--timeout", type=int, default=30)
    parser.add_argument("--append", default="", help="additional explicit host kernel parameters")
    parser.add_argument("--watchdog", choices=("off", "on"), default="off",
                        help="off proves synchronization does not depend on periodic guest exits")
    parser.add_argument("--backend", choices=("tessera", "kvm"), default="tessera")
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    args = parser.parse_args()
    if args.timeout < 1:
        parser.error("timeout must be positive")
    config = args.config.read_text().splitlines()
    if "CONFIG_ARM64_4K_PAGES=y" not in config:
        parser.error("4 KiB ARM64 kernel required")
    if args.backend == "tessera":
        for option in ("CONFIG_AXVISOR_LINUX_BRIDGE=y", "CONFIG_AXVISOR_LINUX_CONTROL=y",
                       "CONFIG_AXVISOR_ARM64_VHE=y"):
            if option not in config:
                parser.error("hosted VHE kernel configuration required")
        if "CONFIG_KVM=y" in config or "CONFIG_KVM=m" in config:
            parser.error("native KVM cannot be substituted for the Tessera endpoint")
    elif "CONFIG_KVM=y" not in config:
        parser.error("built-in native KVM required for native comparison")
    manifest = json.loads((args.assets / "manifest.json").read_text())
    image = args.assets / "initramfs.cpio.gz"
    if sha(image) != manifest["initramfs_sha256"]:
        parser.error("initramfs hash does not match its manifest")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    append = "console=ttyAMA0 earlycon=pl011,0x9000000 rdinit=/init panic=-1 loglevel=7"
    if args.backend == "tessera":
        append += " axvisor_linux.control=1 axvisor_linux.guest_cpus=2-3"
        append += " axvisor_linux.watchdog=" + ("0" if args.watchdog == "off" else "1")
    append += " " + args.append
    command = [args.qemu, "-machine", "virt,virtualization=on,gic-version=3",
               "-cpu", "cortex-a76", "-accel", "tcg,thread=multi", "-smp", "4",
               "-m", "1024", "-display", "none", "-monitor", "none", "-serial", "stdio",
               "-no-reboot", "-kernel", str(args.kernel.resolve()),
               "-initrd", str(image.resolve()), "-append", append.strip()]
    metadata = {"command": command, "manifest": manifest, "backend": args.backend,
                "watchdog": args.watchdog if args.backend == "tessera" else "native",
                "scope": "one VM, two vCPUs, functional PSCI/SMCCC and stop synchronization",
                "kernel_sha256": sha(args.kernel), "config_sha256": sha(args.config),
                "runner_sha256": sha(Path(__file__)),
                "qemu_version": subprocess.check_output([args.qemu, "--version"], text=True).splitlines()[0]}
    (out / "run.json").write_text(json.dumps(metadata, indent=2) + "\n")
    (out / "host.config").write_bytes(args.config.read_bytes())
    started = time.monotonic()
    stopped = timed_out = False
    log_path = out / "console.log"
    with log_path.open("wb") as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        while process.poll() is None:
            stopped = bool(re.search(FAULTS, log_path.read_text(errors="replace"), re.I))
            timed_out = time.monotonic() - started > args.timeout
            if stopped or timed_out:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                break
            time.sleep(0.05)
        returncode = process.wait()
    text = log_path.read_text(errors="replace")
    required_once = ("UNKNOWN_PSCI_HVC", "UNKNOWN_PSCI_SMC", "UNKNOWN_SMC",
                     "NONZERO_SMC_IMMEDIATE", "PRIVATE_HVC_SEPARATE", "CPU_ON_ALREADY_ON",
                     "HVC_OFF", "HVC_RESET", "SMC_OFF", "SMC_RESET", "FD_BALANCE")
    checks = {name: len(re.findall(r"^" + name + r"=PASS\r?$", text, re.M)) == 1
              for name in required_once}
    for name in ("SYNCHRONOUS_STOP", "STOPPED_RUN_EINTR", "EXPLICIT_RESUME_INTERNAL_FAILURE"):
        checks[name] = len(re.findall(r"^" + name + r"=PASS\r?$", text, re.M)) == 4
    checks.update({"natural_zero_exit": returncode == 0 and not stopped and not timed_out,
                   "result": "ARM64_PSCI_RESULT=PASS" in text,
                   "no_faults": not re.search(FAULTS, text, re.I)})
    if args.backend == "tessera":
        checks["hosted_endpoint"] = "AxVisor KVM control endpoint registered: kvm" in text
    verdict = {"pass": all(checks.values()), "checks": checks,
               "faults": re.findall(FAULTS, text, re.I), "timed_out": timed_out,
               "stopped_on_fault": stopped, "returncode": returncode,
               "elapsed_seconds": time.monotonic() - started}
    (out / "verdict.json").write_text(json.dumps(verdict, indent=2) + "\n")
    print(json.dumps(verdict, indent=2))
    raise SystemExit(0 if verdict["pass"] else 1)


if __name__ == "__main__":
    main()
