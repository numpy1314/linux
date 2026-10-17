#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Extract the adapter's actual allocator into a disposable kernel test tree."""
import argparse
import hashlib
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--linux-source", required=True, type=Path)
    parser.add_argument("--runtime-source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    linux = args.linux_source.resolve()
    source = args.runtime_source.resolve()
    output = args.output.resolve()
    makefile = linux / "lib/Makefile"
    if not makefile.is_file() or not (linux / "Makefile").is_file():
        parser.error("--linux-source must be a disposable Linux source tree")
    raw = source.read_bytes()
    text = raw.decode()
    start = text.index("/* Ordinary Rust heap storage")
    end = text.index("struct axvisor_io_map", start)
    allocator = text[start:end]
    for signature in ("static bool axvisor_linux_heap_atomic(void)",
                      "void *axvisor_linux_alloc(size_t size, size_t align)",
                      "void axvisor_linux_dealloc(void *ptr)"):
        if allocator.count(signature) != 1:
            parser.error("unexpected allocator layout: " + signature)
    template = Path(__file__).with_name("fh_heap_test.c.in").read_text()
    if template.count("/* EXTRACTED_ADAPTER_ALLOCATOR */") != 1:
        parser.error("invalid harness template")
    generated = template.replace("/* EXTRACTED_ADAPTER_ALLOCATOR */", allocator)
    output.mkdir(parents=True, exist_ok=True)
    (output / "allocator-exact.c").write_text(allocator)
    (output / "fh_heap_test.c").write_text(generated)
    provenance = {
        "runtime_filename": source.name,
        "runtime_sha256": hashlib.sha256(raw).hexdigest(),
        "allocator_sha256": hashlib.sha256(allocator.encode()).hexdigest(),
        "generated_test_sha256": hashlib.sha256(generated.encode()).hexdigest(),
        "kernel_makefile_sha256": hashlib.sha256((linux / "Makefile").read_bytes()).hexdigest(),
        "scope": "actual adapter heap callbacks in a real kernel; no VM lifecycle claim",
    }
    (output / "source-provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
    (linux / "lib/fh_heap_test.c").write_text(generated)
    content = makefile.read_text()
    line = "obj-y += fh_heap_test.o"
    if line not in content.splitlines():
        makefile.write_text(content.rstrip() + "\n\n" + line + "\n")
    print("Generated and installed lib/fh_heap_test.c in the disposable kernel source")
    print("RUNTIME_SHA256=" + provenance["runtime_sha256"])


if __name__ == "__main__":
    main()
