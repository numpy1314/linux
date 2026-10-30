#!/usr/bin/env python3
"""Build the freestanding ARM64 same-CPU signal liveness test /init and deterministic initramfs."""
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
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--cpu", type=int, default=0)
    args = parser.parse_args()
    if args.rounds <= 0 or not 0 <= args.cpu < 64:
        parser.error("rounds must be positive and cpu in 0..63")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).resolve().parent / "init.c"
    binary = out / "arm64_liveness"
    command = [args.cc, "-O2", "-Wall", "-Wextra", "-Werror", "-ffreestanding",
               "-fno-builtin", "-fno-stack-protector", "-mgeneral-regs-only",
               "-nostdlib", "-static", "-no-pie", "-Wl,-e,_start,--build-id=none",
               f"-DLIVENESS_ROUNDS={args.rounds}", f"-DLIVENESS_CPU={args.cpu}",
               str(source), "-o", str(binary)]
    subprocess.run(command, check=True)
    archive = b"".join(entry(name, stat.S_IFDIR | 0o755, ino=i)
                       for i, name in enumerate(("dev", "proc", "sys", "tmp"), 1))
    archive += entry("dev/console", stat.S_IFCHR | 0o600, major=5, minor=1, ino=5)
    archive += entry("init", stat.S_IFREG | 0o755, binary.read_bytes(), ino=6)
    archive += entry("TRAILER!!!", 0, ino=0)
    image = out / "initramfs.cpio.gz"
    image.write_bytes(gzip.compress(archive, mtime=0))
    manifest = {"command": command, "rounds": args.rounds, "cpu": args.cpu,
                "compiler": subprocess.check_output([args.cc, "--version"], text=True).splitlines()[0],
                "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                "initramfs_sha256": hashlib.sha256(image.read_bytes()).hexdigest()}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(image)


if __name__ == "__main__":
    main()
