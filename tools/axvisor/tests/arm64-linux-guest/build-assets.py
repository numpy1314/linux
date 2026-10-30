#!/usr/bin/env python3
"""Build a Firecracker/Linux guest smoke from verified upstream binaries."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re
import stat
import subprocess
import tarfile


def digest(path):
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


def entry(name, mode, data=b"", major=0, minor=0, ino=1):
    encoded = name.encode() + b"\0"
    fields = (ino, mode, 0, 0, 1, 0, len(data), 0, 0, major, minor, len(encoded), 0)
    result = b"070701" + b"".join(f"{value:08x}".encode() for value in fields) + encoded
    result += b"\0" * (-len(result) % 4)
    result += data
    return result + b"\0" * (-len(data) % 4)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--vcpus", type=int, choices=(1, 2, 4), default=1)
    parser.add_argument("--guest-shutdown", choices=("restart", "poweroff"), default="restart")
    parser.add_argument("--fc-cpus", type=cpu_list,
                        help="host CPUs allowed for the whole Firecracker process (e.g. 2-3)")
    parser.add_argument("--cc", default="aarch64-linux-gnu-gcc")
    args = parser.parse_args()
    if args.rounds < 1:
        parser.error("rounds must be positive")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    archive = args.assets / "firecracker-v1.10.1-aarch64.tgz"
    kernel = args.assets / "Image-5.10.223"
    expected = {
        archive: "9e3641071de140979afaac0c52fdc107baeba398bdb5709c12f77ee469207fcd",
        kernel: "eb5d95ac8a67f7a86acf0cb35625633713ad5170b56de8617808d0e18bb832ec",
    }
    for path, sha in expected.items():
        if digest(path) != sha:
            raise SystemExit(f"Upstream artifact hash mismatch: {path}")
    binary = out / "firecracker"
    with tarfile.open(archive) as tar:
        binary.write_bytes(tar.extractfile("release-v1.10.1-aarch64/firecracker-v1.10.1-aarch64").read())
    if digest(binary) != "ff62dd7a90db03a056e61e94058b9439f47182809926545a5b555879f7816506":
        raise SystemExit("Firecracker binary hash mismatch")
    source = Path(__file__).with_name("init.c")
    common = [args.cc, "-O2", "-Wall", "-Wextra", "-Werror", "-ffreestanding",
              "-fno-builtin", "-fno-stack-protector", "-mgeneral-regs-only",
              "-nostdlib", "-static", "-no-pie", "-Wl,-e,_start,--build-id=none"]
    commands = []
    reboot_command = "0x01234567" if args.guest_shutdown == "restart" else "0x4321fedc"
    fc_mask = sum(1 << cpu for cpu in (args.fc_cpus or []))
    for kind, extra in (("host", [f"-DTEST_ROUNDS={args.rounds}", f"-DFC_CPU_MASK={fc_mask}UL"]),
                        ("guest", ["-DGUEST_INIT", f"-DGUEST_VCPUS={args.vcpus}",
                                   f"-DGUEST_REBOOT_COMMAND={reboot_command}"])):
        command = common + extra + [str(source), "-o", str(out / f"{kind}-init")]
        subprocess.run(command, check=True)
        commands.append(command)
    root = out / "guest-root"
    root.mkdir(exist_ok=True)
    for directory in ("dev", "proc", "sys", "tmp"):
        (root / directory).mkdir(exist_ok=True)
    (root / "init").write_bytes((out / "guest-init").read_bytes())
    (root / "init").chmod(0o755)
    disk = out / "guest.ext4"
    with disk.open("wb") as stream:
        stream.truncate(64 * 1024 * 1024)
    command = ["mke2fs", "-q", "-F", "-t", "ext4", "-b", "4096",
               "-O", "^orphan_file,^metadata_csum_seed", "-d", str(root), str(disk)]
    subprocess.run(command, check=True)
    commands.append(command)
    config = {
        "boot-source": {"kernel_image_path": "/Image-5.10.223",
                        "boot_args": "console=ttyS0 reboot=k panic=1 pci=off root=/dev/vda rw init=/init"},
        "drives": [{"drive_id": "rootfs", "path_on_host": "/guest.ext4",
                    "is_root_device": True, "is_read_only": False}],
        "machine-config": {"vcpu_count": args.vcpus, "mem_size_mib": 512, "smt": False},
        "logger": {"log_path": "/dev/console", "level": "Info", "show_level": True,
                   "show_log_origin": False},
    }
    config_path = out / "firecracker.json"
    config_path.write_text(json.dumps(config, indent=2) + "\n")
    packed = b"".join(entry(name, stat.S_IFDIR | 0o755, ino=i)
                       for i, name in enumerate(("dev", "proc", "sys", "tmp"), 1))
    packed += entry("dev/console", stat.S_IFCHR | 0o600, major=5, minor=1, ino=5)
    for i, (name, path, mode) in enumerate((
            ("init", out / "host-init", 0o755), ("firecracker", binary, 0o755),
            (kernel.name, kernel, 0o644), (disk.name, disk, 0o644),
            (config_path.name, config_path, 0o644)), 6):
        packed += entry(name, stat.S_IFREG | mode, path.read_bytes(), ino=i)
    packed += entry("TRAILER!!!", 0, ino=0)
    initramfs = out / "initramfs.cpio.gz"
    initramfs.write_bytes(gzip.compress(packed, mtime=0))
    manifest = {"rounds": args.rounds, "guest_vcpus": args.vcpus, "guest_memory_mib": 512,
                "guest_shutdown": args.guest_shutdown,
                "firecracker_cpu_affinity": args.fc_cpus,
                "commands": commands,
                "compiler": subprocess.check_output([args.cc, "--version"], text=True).splitlines()[0],
                "files": {path.name: {"bytes": path.stat().st_size, "sha256": digest(path)}
                          for path in (archive, kernel, source, Path(__file__), binary, out / "host-init",
                                       out / "guest-init", disk, config_path, initramfs)},
                "scope": "functional Linux guest CPU/timer/file write-fsync-reopen-read and VMM exit; no performance claim"}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(initramfs)


if __name__ == "__main__":
    main()
