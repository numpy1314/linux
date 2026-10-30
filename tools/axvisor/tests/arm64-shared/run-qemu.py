#!/usr/bin/env python3
"""Run the shared-Core ARM64 ioctl smoke in an isolated QEMU host."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time

FAULT_PATTERN = r"ARM64_KVM_SMOKE_FAIL|ARM64_KVM_SMOKE_RESULT=FAIL|POWEROFF_ERROR|Kernel panic|core panic:|panicked at|BUG:|WARNING:|Oops:|Call trace:|soft lockup|rcu[^\n]*stall"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", required=True, type=Path)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--initramfs", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--cpus", type=int, choices=(2, 4), default=2)
    parser.add_argument("--memory-mib", type=int, default=1024, help="QEMU host RAM, independent of guest RAM")
    parser.add_argument("--rounds", type=int, help="expected rounds; defaults to the verified initramfs manifest")
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--qemu", default="qemu-system-aarch64")
    args = parser.parse_args()
    if args.memory_mib <= 0 or args.timeout <= 0 or (args.rounds is not None and args.rounds <= 0):
        parser.error("host memory, timeout, and round count must be positive")
    image_hash = hashlib.sha256(args.initramfs.read_bytes()).hexdigest()
    manifest_path = args.initramfs.parent / "manifest.json"
    manifest = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
    if manifest and manifest.get("initramfs_sha256") != image_hash:
        raise SystemExit("Initramfs hash does not match its manifest")
    rounds = args.rounds if args.rounds is not None else manifest.get("rounds")
    if not isinstance(rounds, int) or rounds <= 0:
        parser.error("provide a manifest with rounds or an explicit --rounds")
    if manifest.get("rounds", rounds) != rounds:
        parser.error("--rounds disagrees with the initramfs manifest")
    guest_cpus = manifest.get("guest_cpus", [])
    if guest_cpus and (any(cpu < 0 or cpu >= args.cpus for cpu in guest_cpus) or
                       manifest.get("outside_cpu") not in range(args.cpus)):
        parser.error("manifest pool/outside CPU must exist in the selected QEMU host")
    config = args.config.read_text().splitlines()
    required = ("CONFIG_ARM64_4K_PAGES=y", "CONFIG_AXVISOR_LINUX_BRIDGE=y",
                "CONFIG_AXVISOR_LINUX_CONTROL=y", "CONFIG_AXVISOR_ARM64_VHE=y")
    forbidden = ("CONFIG_KVM=y", "CONFIG_KVM=m", "CONFIG_HOTPLUG_CPU=y",
                 "CONFIG_ARM64_PSEUDO_NMI=y", "CONFIG_ARM64_MTE=y",
                 "CONFIG_ARM64_SME=y", "CONFIG_ARM64_POE=y")
    if any(line not in config for line in required) or any(line in config for line in forbidden):
        raise SystemExit("Unsupported kernel config: require hosted VHE, 4K pages, no native KVM/hotplug/unsupported state")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    boot_args = "console=ttyAMA0 earlycon=pl011,0x9000000 rdinit=/init panic=-1 loglevel=7 axvisor_linux.control=1"
    if guest_cpus:
        boot_args += " axvisor_linux.guest_cpus=" + ",".join(map(str, guest_cpus))
    command = [args.qemu, "-machine", "virt,virtualization=on,gic-version=3",
               "-cpu", "cortex-a76", "-accel", "tcg,thread=multi", "-smp", str(args.cpus),
               "-m", str(args.memory_mib), "-display", "none", "-monitor", "none", "-serial", "stdio",
               "-no-reboot", "-kernel", str(args.kernel.resolve()),
               "-initrd", str(args.initramfs.resolve()), "-append",
               boot_args]
    metadata = {"command": command, "scope": "shared-Core KVM ioctl/MMIO/FD lifecycle smoke",
                "kernel_sha256": hashlib.sha256(args.kernel.read_bytes()).hexdigest(),
                "config_sha256": hashlib.sha256(args.config.read_bytes()).hexdigest(),
                "initramfs_sha256": image_hash, "expected_rounds": rounds,
                "guest_memory_bytes": manifest.get("memory_bytes"), "host_memory_mib": args.memory_mib,
                "guest_cpus": guest_cpus, "outside_cpu": manifest.get("outside_cpu"),
                "qemu_version": subprocess.check_output([args.qemu, "--version"], text=True).splitlines()[0]}
    (out / "run.json").write_text(json.dumps(metadata, indent=2) + "\n")
    started = time.monotonic()
    timed_out = False
    stopped_on_fault = False
    with (out / "console.log").open("wb") as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        while process.poll() is None:
            current_log = (out / "console.log").read_text(errors="replace")
            stopped_on_fault = bool(re.search(FAULT_PATTERN, current_log, flags=re.IGNORECASE))
            timed_out = time.monotonic() - started >= args.timeout
            if stopped_on_fault or timed_out:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                break
            time.sleep(0.1)
        returncode = process.wait()
    text = (out / "console.log").read_text(errors="replace")
    faults = re.findall(FAULT_PATTERN, text, flags=re.IGNORECASE)
    teardown_ns = [int(value) for value in re.findall(r"^DESTROY_NS=(\d+)\r?$", text, flags=re.MULTILINE)]
    reported_memory = re.findall(r"^GUEST_MEMORY_BYTES=(\d+)\r?$", text, flags=re.MULTILINE)
    checks = {"qemu_exited_zero": returncode == 0, "no_timeout": not timed_out,
              "hosted_endpoint_registered": "AxVisor KVM control endpoint registered: kvm" in text,
              "rounds": re.findall(r"^ROUND_PASS=(\d+)\r?$", text, flags=re.MULTILINE) == [str(i) for i in range(1, rounds + 1)],
              "declared_rounds": re.findall(r"^TEST_ROUNDS=(\d+)\r?$", text, flags=re.MULTILINE) == [str(rounds)],
              "guest_memory": reported_memory == [str(manifest["memory_bytes"])] if "memory_bytes" in manifest else len(reported_memory) == 1,
              "fault_retries": text.count("CREATE_DEVICE_EFAULT_RETRY=PASS") == rounds,
              "mmio_writes": text.count("MMIO_WRITE=PASS") == rounds,
              "round_fd_balance": text.count("ROUND_FD_BALANCE=PASS") == rounds,
              "teardown_measurements": len(teardown_ns) == rounds,
              "fd_balance": len(re.findall(r"^FD_BALANCE=PASS\r?$", text, flags=re.MULTILINE)) == 1,
              "test_success": "ARM64_KVM_SMOKE_RESULT=PASS" in text,
              "no_fault_markers": not faults}
    if guest_cpus:
        checks.update({
            "cpu_pool_declaration": re.findall(r"^TEST_CPU_MASK=(\d+)\r?$", text, flags=re.MULTILINE) == [str(manifest["cpu_mask"])],
            "pool_create_coverage": set(map(int, re.findall(r"^CREATE_VM_CPU=(\d+)\r?$", text, flags=re.MULTILINE))) == set(guest_cpus),
            "pool_run_coverage": set(map(int, re.findall(r"^RUN_CPU=(\d+)\r?$", text, flags=re.MULTILINE))) == set(guest_cpus),
            "pool_rotation": len(re.findall(r"^POOL_ROTATION=PASS\r?$", text, flags=re.MULTILINE)) == 1,
            "outside_pool_rejected": len(re.findall(r"^OFFPOOL_IOCTL=PASS\r?$", text, flags=re.MULTILINE)) == 1,
            "outside_pool_close": len(re.findall(r"^OFFPOOL_CLOSE=PASS\r?$", text, flags=re.MULTILINE)) == rounds,
        })
    verdict = {"pass": all(checks.values()), "checks": checks, "fault_markers": faults,
               "returncode": returncode, "timed_out": timed_out,
               "stopped_on_fault": stopped_on_fault,
               "teardown_ns": teardown_ns,
               "elapsed_seconds": time.monotonic() - started}
    (out / "verdict.json").write_text(json.dumps(verdict, indent=2) + "\n")
    print(json.dumps(verdict, indent=2))
    raise SystemExit(0 if verdict["pass"] else 1)


if __name__ == "__main__":
    main()
