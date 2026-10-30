#!/usr/bin/env python3
"""Run fresh Firecracker/Linux guest functional smoke on the hosted Core."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def cpu_list(value):
    cpus = set()
    for part in value.split(","):
        if not re.fullmatch(r"[0-9]+(?:-[0-9]+)?", part):
            raise argparse.ArgumentTypeError("CPU list must look like 0,2-3")
        bounds = [int(n) for n in part.split("-")]
        start, end = bounds[0], bounds[-1]
        if start > end or end > 63:
            raise argparse.ArgumentTypeError("CPU IDs must be ascending ranges within 0..63")
        cpus.update(range(start, end + 1))
    return sorted(cpus)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", required=True, type=Path)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--assets", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--cpus", type=int, choices=(2, 4), default=2,
                        help="outer QEMU host CPU count; guest count comes from the asset manifest")
    parser.add_argument("--guest-cpus", type=cpu_list,
                        help="host CPU pool admitted by axvisor_linux.guest_cpus, e.g. 2-3")
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    args = parser.parse_args()
    if args.timeout < 1:
        parser.error("timeout must be positive")
    config = args.config.read_text().splitlines()
    required = ("CONFIG_ARM64_4K_PAGES=y", "CONFIG_AXVISOR_LINUX_BRIDGE=y",
                "CONFIG_AXVISOR_LINUX_CONTROL=y", "CONFIG_AXVISOR_ARM64_VHE=y")
    forbidden = ("CONFIG_KVM=y", "CONFIG_KVM=m", "CONFIG_HOTPLUG_CPU=y",
                 "CONFIG_ARM64_PSEUDO_NMI=y", "CONFIG_ARM64_MTE=y",
                 "CONFIG_ARM64_SME=y", "CONFIG_ARM64_POE=y")
    if any(line not in config for line in required) or any(line in config for line in forbidden):
        raise SystemExit("Kernel config is not the admitted hosted VHE profile")
    manifest_path = args.assets / "manifest.json"
    manifest = json.loads(manifest_path.read_text())
    host_cpus = set(range(args.cpus))
    pool = set(args.guest_cpus) if args.guest_cpus is not None else host_cpus
    fc_cpus = manifest.get("firecracker_cpu_affinity")
    fc_cpus = set(fc_cpus) if fc_cpus is not None else host_cpus
    if not pool or not pool <= host_cpus or not fc_cpus or not fc_cpus <= pool:
        parser.error("Firecracker affinity must be a nonempty subset of the admitted online host CPU pool")
    expected_fc_mask = sum(1 << cpu for cpu in fc_cpus)
    initramfs = args.assets / "initramfs.cpio.gz"
    if sha(initramfs) != manifest["files"][initramfs.name]["sha256"]:
        raise SystemExit("Initramfs hash differs from its build manifest")
    rounds = manifest["rounds"]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    (out / "host.config").write_bytes(args.config.read_bytes())
    append = "console=ttyAMA0 earlycon=pl011,0x9000000 rdinit=/init panic=-1 loglevel=7 axvisor_linux.control=1"
    if args.guest_cpus is not None:
        append += " axvisor_linux.guest_cpus=" + ",".join(str(cpu) for cpu in sorted(pool))
    command = [args.qemu, "-machine", "virt,virtualization=on,gic-version=3",
               "-cpu", "cortex-a76", "-accel", "tcg,thread=multi", "-smp", str(args.cpus),
               "-m", "2048", "-display", "none", "-monitor", "none", "-serial", "stdio",
               "-no-reboot", "-kernel", str(args.kernel.resolve()),
               "-initrd", str(initramfs.resolve()), "-append", append]
    metadata = {"command": command, "scope": manifest["scope"],
                "kernel_sha256": sha(args.kernel), "config_sha256": sha(args.config),
                "runner_sha256": sha(Path(__file__)),
                "initramfs_sha256": sha(initramfs), "asset_manifest": manifest,
                "host_cpus": args.cpus, "guest_cpu_pool": sorted(pool),
                "firecracker_cpu_affinity": sorted(fc_cpus),
                "qemu_version": subprocess.check_output([args.qemu, "--version"], text=True).splitlines()[0]}
    (out / "run.json").write_text(json.dumps(metadata, indent=2) + "\n")
    started = time.monotonic()
    timed_out = stopped_on_fault = False
    fault_pattern = (r"FH_LINUX_(?:GUEST|HOST)_FAIL|FH_LINUX_(?:GUEST|HOST)_RESULT=FAIL|"
                     r"FIRECRACKER_EXEC_ERROR|REBOOT_ERROR|Reboot failed|Kernel panic|core panic:|panicked at|"
                     r"BUG:|WARNING:|Oops:|Call trace:|soft lockup|rcu[^\n]*stall")
    log_path = out / "console.log"
    with log_path.open("wb") as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        while process.poll() is None:
            if time.monotonic() - started > args.timeout:
                timed_out = True
                process.kill()
                break
            if re.search(fault_pattern, log_path.read_text(errors="replace"), re.IGNORECASE):
                stopped_on_fault = True
                # Let the serial writer finish the fault line (including errno).
                time.sleep(0.5)
                process.terminate()
                break
            time.sleep(0.1)
        try:
            returncode = process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            returncode = process.wait()
    text = log_path.read_text(errors="replace")
    faults = re.findall(fault_pattern, text, re.IGNORECASE)
    checks = {"qemu_exited_zero": returncode == 0, "no_timeout": not timed_out,
              "not_stopped_by_runner": not stopped_on_fault,
              "hosted_endpoint_registered": "AxVisor KVM control endpoint registered: kvm" in text,
              "host_success": "FH_LINUX_HOST_RESULT=PASS" in text,
              "rounds": re.findall(r"^FH_LINUX_ROUND_PASS=(\d+)\r?$", text, re.MULTILINE)
                        == [str(i) for i in range(1, rounds + 1)],
              "guest_successes": text.count("FH_LINUX_GUEST_RESULT=PASS") == rounds,
              "topology_successes": text.count("FH_LINUX_TOPOLOGY=PASS") == rounds,
              "guest_vcpus": re.findall(r"^FH_LINUX_GUEST_VCPUS=(\d+)\r?$", text, re.MULTILINE)
                             == [str(manifest["guest_vcpus"])] * rounds,
              "cpu_successes": text.count("FH_LINUX_CPU=PASS") == rounds,
              "timer_successes": text.count("FH_LINUX_TIMER=PASS") == rounds,
              "io_successes": text.count("FH_LINUX_IO=PASS") == rounds,
              "vmm_clean_exits": text.count("Firecracker exiting successfully. exit_code=0") == rounds,
              "vm_cleanup_completed": text.count("resources cleanup completed") == rounds,
              "firecracker_affinity": re.findall(r"^FH_FC_CPU_MASK=(\d+)\r?$", text, re.MULTILINE)
                                       == [str(expected_fc_mask)] * rounds,
              "firecracker_affinity_checks": text.count("FH_FC_AFFINITY=PASS") == rounds,
              "no_fault_markers": not faults}
    verdict = {"pass": all(checks.values()), "checks": checks, "fault_markers": faults,
               "returncode": returncode, "timed_out": timed_out,
               "stopped_on_fault": stopped_on_fault,
               "elapsed_seconds": time.monotonic() - started}
    if manifest.get("guest_shutdown") == "restart" and "Reboot failed -- System halted" in text:
        verdict["observed_known_limitation"] = (
            "Shared Core 4152 consumes HVC PSCI SYSTEM_RESET with NOT_SUPPORTED; "
            "the existing HostIf/Control API exposes no function-number hook. "
            "This failed restart case is not a lifecycle PASS.")
    (out / "verdict.json").write_text(json.dumps(verdict, indent=2) + "\n")
    print(json.dumps(verdict, indent=2))
    raise SystemExit(0 if verdict["pass"] else 1)


if __name__ == "__main__":
    main()
