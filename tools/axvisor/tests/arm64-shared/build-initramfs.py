#!/usr/bin/env python3
"""Build the freestanding ARM64 ioctl smoke /init and deterministic initramfs."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import stat
import subprocess


def entry(name, mode, data=b"", major=0, minor=0, ino=1):
    name = name.encode() + b"\0"
    fields = (ino, mode, 0, 0, 1, 0, len(data), 0, 0, major, minor, len(name), 0)
    result = b"070701" + b"".join(f"{value:08x}".encode() for value in fields) + name
    result += b"\0" * (-len(result) % 4)
    result += data
    return result + b"\0" * (-len(data) % 4)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--cc", default="aarch64-linux-gnu-gcc")
    parser.add_argument("--memory-mib", type=int, default=0, help="guest RAM in MiB; 0 selects a single 4 KiB page")
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--guest-cpus", help="test an explicit kernel guest CPU pool, e.g. 2-3")
    parser.add_argument("--outside-cpu", type=int, default=0, help="CPU outside the explicit pool for rejection/close tests")
    args = parser.parse_args()
    if args.memory_mib < 0 or args.rounds <= 0:
        parser.error("memory-mib must be nonnegative and rounds must be positive")
    guest_cpus = set()
    try:
        for part in args.guest_cpus.split(",") if args.guest_cpus else ():
            ends = [int(value) for value in part.split("-")]
            if len(ends) > 2 or ends[0] < 0 or ends[-1] >= 64 or ends[0] > ends[-1]:
                raise ValueError
            guest_cpus.update(range(ends[0], ends[-1] + 1))
    except ValueError:
        parser.error("guest-cpus must be a list/range of CPU ids in 0..63")
    if guest_cpus and len(guest_cpus) < 2:
        parser.error("the CPU-pool rotation test requires at least two pool CPUs")
    if guest_cpus and (args.rounds < len(guest_cpus) or not 0 <= args.outside_cpu < 64 or args.outside_cpu in guest_cpus):
        parser.error("cover every pool CPU with at least one round and select a distinct outside-cpu")
    cpu_mask = sum(1 << cpu for cpu in guest_cpus)
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).resolve().parent.parent / "programs/arm64_kvm_smoke.c"
    binary = out / "arm64_kvm_smoke"
    command = [args.cc, "-O2", "-Wall", "-Wextra", "-Werror", "-ffreestanding",
               "-fno-builtin", "-fno-stack-protector", "-mgeneral-regs-only",
               "-nostdlib", "-static", "-no-pie", "-Wl,-e,_start,--build-id=none",
               f"-DSMOKE_MEMORY_MIB={args.memory_mib}", f"-DSMOKE_ROUNDS={args.rounds}",
               f"-DSMOKE_CPU_MASK={cpu_mask}UL", f"-DSMOKE_OUTSIDE_CPU={args.outside_cpu}",
               str(source), "-o", str(binary)]
    subprocess.run(command, check=True)
    archive = b"".join(entry(name, stat.S_IFDIR | 0o755, ino=i)
                       for i, name in enumerate(("dev", "proc", "sys", "tmp"), 1))
    archive += entry("dev/console", stat.S_IFCHR | 0o600, major=5, minor=1, ino=5)
    archive += entry("init", stat.S_IFREG | 0o755, binary.read_bytes(), ino=6)
    archive += entry("TRAILER!!!", 0, ino=0)
    image = out / "initramfs.cpio.gz"
    image.write_bytes(gzip.compress(archive, mtime=0))
    manifest = {"command": command, "memory_mib": args.memory_mib,
                "memory_bytes": args.memory_mib * 1024 * 1024 if args.memory_mib else 4096,
                "rounds": args.rounds,
                "guest_cpus": sorted(guest_cpus), "cpu_mask": cpu_mask,
                "outside_cpu": args.outside_cpu if guest_cpus else None,
                "compiler": subprocess.check_output([args.cc, "--version"], text=True).splitlines()[0],
                "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                "initramfs_sha256": hashlib.sha256(image.read_bytes()).hexdigest()}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(image)


if __name__ == "__main__":
    main()
