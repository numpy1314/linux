#!/usr/bin/env python3
"""Require real Firecracker Finish to stop a guest still executing on another vCPU."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time

FAULTS = r"FC_STOP_(?:GUEST_FAIL|HOST_FAIL|EXEC_ERROR|HOST_RESULT=FAIL)|HOST_POWEROFF_ERROR|Failed to send VcpuEvent::Finish|Failed signaling vcpu exit event|Kernel panic|core panic:|panicked at|BUG:|WARNING:|Oops:|Call trace:|soft lockup|rcu[^\n]*stall"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", required=True, type=Path)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--assets", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--timeout", type=int, default=45)
    parser.add_argument("--watchdog", choices=("off", "on"), default="off")
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    args = parser.parse_args()
    if args.timeout < 1:
        parser.error("timeout must be positive")
    config = args.config.read_text().splitlines()
    required = ("CONFIG_ARM64_4K_PAGES=y", "CONFIG_AXVISOR_LINUX_BRIDGE=y",
                "CONFIG_AXVISOR_LINUX_CONTROL=y", "CONFIG_AXVISOR_ARM64_VHE=y")
    forbidden = ("CONFIG_KVM=y", "CONFIG_KVM=m", "CONFIG_HOTPLUG_CPU=y",
                 "CONFIG_ARM64_PSEUDO_NMI=y", "CONFIG_ARM64_MTE=y", "CONFIG_ARM64_SME=y")
    if any(x not in config for x in required) or any(x in config for x in forbidden):
        parser.error("admitted hosted VHE kernel configuration required")
    manifest = json.loads((args.assets / "manifest.json").read_text())
    image = args.assets / "initramfs.cpio.gz"
    if sha(image) != manifest["files"][image.name]["sha256"]:
        parser.error("initramfs does not match its build manifest")
    cpus = manifest["firecracker_cpu_affinity"]
    if cpus not in ([2], [2, 3]) or manifest["accepted_firecracker_exit_codes"] != [0, 1]:
        parser.error("unsupported affinity or expected Firecracker exit")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    append = "console=ttyAMA0 earlycon=pl011,0x9000000 rdinit=/init panic=-1 loglevel=7 "
    append += "axvisor_linux.control=1 axvisor_linux.guest_cpus=2-3 "
    append += "axvisor_linux.watchdog=" + ("0" if args.watchdog == "off" else "1")
    command = [args.qemu, "-machine", "virt,virtualization=on,gic-version=3", "-cpu", "cortex-a76",
               "-accel", "tcg,thread=multi", "-smp", "4", "-m", "2048", "-display", "none",
               "-monitor", "none", "-serial", "stdio", "-no-reboot",
               "-kernel", str(args.kernel.resolve()), "-initrd", str(image.resolve()), "-append", append]
    metadata = {"command": command, "manifest": manifest, "watchdog": args.watchdog,
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
    markers = ("FC_STOP_AFFINITY", "FC_STOP_BUSY_COUNTER", "FC_STOP_EXPECTED_EXIT",
               "FC_STOP_FD_BALANCE", "FC_STOP_HOST_AFTER_EXIT")
    checks = {name: len(re.findall(r"^" + name + r"=PASS\r?$", text, re.M)) == 1 for name in markers}
    checks.update({"natural_outer_exit": returncode == 0 and not stopped and not timed_out,
                   "host_result": "FC_STOP_HOST_RESULT=PASS" in text,
                   "hosted_endpoint": "AxVisor KVM control endpoint registered: kvm" in text,
                   "expected_unknown_exit": "Unexpected exit reason on vcpu run: Unknown" in text,
                   "vmm_stop": "Vmm is stopping." in text,
                   "normal_fc_wait_status": re.findall(r"^FC_STOP_WAIT_STATUS=(\d+)\r?$", text, re.M)
                                            in (["0"], ["256"]),
                   "fc_exit_code": re.findall(r"^FC_STOP_EXIT_CODE=(\d+)\r?$", text, re.M)
                                   in (["0"], ["1"]),
                   "vm_cleanup": text.count("resources cleanup completed") == 1,
                   "no_psci_terminal_exit": "Received KVM_SYSTEM_EVENT" not in text and
                                            "Received KVM_EXIT_SHUTDOWN" not in text and
                                            "Received KVM_EXIT_HLT" not in text,
                   "affinity_mask": re.findall(r"^FC_STOP_CPU_MASK=(\d+)\r?$", text, re.M)
                                    == [str(manifest["firecracker_cpu_mask"])],
                   "no_faults": not re.search(FAULTS, text, re.I)})
    verdict = {"pass": all(checks.values()), "checks": checks,
               "faults": re.findall(FAULTS, text, re.I), "returncode": returncode,
               "timed_out": timed_out, "stopped_on_fault": stopped,
               "elapsed_seconds": time.monotonic() - started,
               "accepted_vmm_exit_codes": [0, 1],
               "observed_vmm_exit_codes": re.findall(r"^FC_STOP_EXIT_CODE=(\d+)\r?$", text, re.M)}
    (out / "verdict.json").write_text(json.dumps(verdict, indent=2) + "\n")
    print(json.dumps(verdict, indent=2))
    raise SystemExit(0 if verdict["pass"] else 1)


if __name__ == "__main__":
    main()
