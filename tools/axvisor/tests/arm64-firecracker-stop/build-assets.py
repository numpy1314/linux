#!/usr/bin/env python3
"""Build an actual Firecracker busy-vCPU stop test without a Linux guest disk."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import stat
import struct
import subprocess
import tarfile


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def entry(name, mode, data=b"", major=0, minor=0, ino=1):
    name = name.encode() + b"\0"
    fields = (ino, mode, 0, 0, 1, 0, len(data), 0, 0, major, minor, len(name), 0)
    result = b"070701" + b"".join(f"{n:08x}".encode() for n in fields) + name
    result += b"\0" * (-len(result) % 4)
    result += data
    return result + b"\0" * (-len(data) % 4)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--assets", required=True, type=Path,
                        help="directory containing the pinned Firecracker v1.10.1 archive")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--fc-cpus", choices=("2-3", "2"), default="2-3")
    parser.add_argument("--cross-compile", default="aarch64-linux-gnu-")
    args = parser.parse_args()
    source = Path(__file__).resolve().parent
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    archive = args.assets / "firecracker-v1.10.1-aarch64.tgz"
    if sha(archive) != "9e3641071de140979afaac0c52fdc107baeba398bdb5709c12f77ee469207fcd":
        raise SystemExit("Firecracker archive hash mismatch")
    firecracker = out / "firecracker"
    with tarfile.open(archive) as tar:
        firecracker.write_bytes(tar.extractfile("release-v1.10.1-aarch64/firecracker-v1.10.1-aarch64").read())
    if sha(firecracker) != "ff62dd7a90db03a056e61e94058b9439f47182809926545a5b555879f7816506":
        raise SystemExit("Firecracker binary hash mismatch")
    commands = []

    def run(command):
        commands.append(list(map(str, command)))
        subprocess.run(command, check=True)

    guest_object, guest_elf, guest_image = out / "guest.o", out / "guest.elf", out / "guest.Image"
    run([args.cross_compile + "gcc", "-c", "-nostdlib", "-o", guest_object, source / "guest.S"])
    run([args.cross_compile + "ld", "-Ttext=0", "-e", "_start", "-o", guest_elf, guest_object])
    run([args.cross_compile + "objcopy", "-O", "binary", guest_elf, guest_image])
    data = guest_image.read_bytes()
    if len(data) < 64 or data[56:60] != b"ARM\x64" or struct.unpack_from("<Q", data, 16)[0] != len(data):
        raise SystemExit("Malformed AArch64 Linux Image header")
    cpus = [2, 3] if args.fc_cpus == "2-3" else [2]
    mask = sum(1 << cpu for cpu in cpus)
    host = out / "host-init"
    run([args.cross_compile + "gcc", "-O2", "-Wall", "-Wextra", "-Werror", "-ffreestanding",
         "-fno-builtin", "-fno-stack-protector", "-mgeneral-regs-only", "-nostdlib", "-static",
         "-no-pie", "-Wl,-e,_start,--build-id=none", f"-DFC_CPU_MASK={mask}UL",
         source / "init.c", "-o", host])
    config = {"boot-source": {"kernel_image_path": "/guest.Image", "boot_args": "console=ttyS0"},
              "machine-config": {"vcpu_count": 2, "mem_size_mib": 512, "smt": False},
              "drives": [],
              "logger": {"log_path": "/dev/console", "level": "Info", "show_level": True,
                         "show_log_origin": False}}
    config_path = out / "firecracker.json"
    config_path.write_text(json.dumps(config, indent=2) + "\n")
    packed = b"".join(entry(n, stat.S_IFDIR | 0o755, ino=i)
                      for i, n in enumerate(("dev", "proc", "sys", "tmp"), 1))
    packed += entry("dev/console", stat.S_IFCHR | 0o600, major=5, minor=1, ino=5)
    for i, (name, path, mode) in enumerate((("init", host, 0o755), ("firecracker", firecracker, 0o755),
                                          ("guest.Image", guest_image, 0o644),
                                          ("firecracker.json", config_path, 0o644)), 6):
        packed += entry(name, stat.S_IFREG | mode, path.read_bytes(), ino=i)
    packed += entry("TRAILER!!!", 0, ino=0)
    initramfs = out / "initramfs.cpio.gz"
    initramfs.write_bytes(gzip.compress(packed, mtime=0))
    files = (archive, source / "guest.S", source / "init.c", Path(__file__),
             guest_elf, guest_image, host, firecracker, config_path, initramfs)
    manifest = {"commands": commands, "guest_vcpus": 2, "guest_memory_mib": 512,
                "firecracker_cpu_affinity": cpus, "firecracker_cpu_mask": mask,
                "accepted_firecracker_exit_codes": [0, 1], "lifecycles": 1,
                "compiler": subprocess.check_output([args.cross_compile + "gcc", "--version"], text=True).splitlines()[0],
                "files": {p.name: {"sha256": sha(p), "bytes": p.stat().st_size} for p in files},
                "scope": "actual Firecracker UNKNOWN-exit -> Finish/signal -> busy-vCPU join/cleanup; no PSCI terminal event"}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(initramfs)


if __name__ == "__main__":
    main()
