#!/usr/bin/env python3
"""Build a one-lifecycle ARM64 PSCI/SMCCC ioctl test initramfs."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import stat
import subprocess


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
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--cc", default="aarch64-linux-gnu-gcc")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).with_name("init.c").resolve()
    binary = out / "arm64_psci"
    command = [args.cc, "-O2", "-Wall", "-Wextra", "-Werror", "-ffreestanding",
               "-fno-builtin", "-fno-stack-protector", "-mgeneral-regs-only", "-mno-outline-atomics",
               "-nostdlib", "-static", "-no-pie", "-Wl,-e,_start,--build-id=none",
               str(source), "-o", str(binary)]
    subprocess.run(command, check=True)
    packed = b"".join(entry(name, stat.S_IFDIR | 0o755, ino=i)
                      for i, name in enumerate(("dev", "proc", "sys", "tmp"), 1))
    packed += entry("dev/console", stat.S_IFCHR | 0o600, major=5, minor=1, ino=5)
    packed += entry("init", stat.S_IFREG | 0o755, binary.read_bytes(), ino=6)
    packed += entry("TRAILER!!!", 0, ino=0)
    image = out / "initramfs.cpio.gz"
    image.write_bytes(gzip.compress(packed, mtime=0))
    manifest = {"command": command, "guest_vcpus": 2, "guest_memory_bytes": 8192,
                "host_cpu_pool": [2, 3], "lifecycles": 1,
                "compiler": subprocess.check_output([args.cc, "--version"], text=True).splitlines()[0],
                "source_sha256": sha(source), "builder_sha256": sha(Path(__file__)),
                "binary_sha256": sha(binary), "initramfs_sha256": sha(image)}
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(image)


if __name__ == "__main__":
    main()
