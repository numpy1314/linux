#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Install exact adapter fd callbacks in a disposable Linux test tree."""
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
    if not (linux / "fs/eventfd.c").is_file():
        parser.error("--linux-source must be a disposable Linux source tree")
    raw = source.read_bytes()
    text = raw.decode()
    start = text.index("struct axvisor_linux_control_fd_ref {")
    end = text.index("\n};", start) + len("\n};")
    fields = text[start:end]
    start = text.index("#if defined(CONFIG_AXVISOR_ARM64_VHE) && defined(CONFIG_EVENTFD)")
    end = text.index("u64 axvisor_linux_control_retain_user_address_space", start)
    callbacks = text[start:end]
    for name in ("get_fd_ref", "read_fd_ref", "write_fd_ref", "wait_fd_readable",
                 "cancel_fd_wait", "release_fd_ref"):
        if callbacks.count("axvisor_linux_control_" + name + "(") != 1:
            parser.error("unexpected callback layout: " + name)
    extracted = fields + "\n\n" + callbacks
    template = Path(__file__).with_name("fh_irqfd_test.c.in").read_text()
    generated = template.replace("/* EXTRACTED_FD_CALLBACKS */", extracted)
    output.mkdir(parents=True, exist_ok=True)
    (output / "fd-callbacks-exact.c").write_text(extracted)
    (output / "source-provenance.json").write_text(json.dumps({
        "runtime_sha256": hashlib.sha256(raw).hexdigest(),
        "callbacks_sha256": hashlib.sha256(extracted.encode()).hexdigest(),
        "generated_sha256": hashlib.sha256(generated.encode()).hexdigest(),
        "scope": "real eventfd and waitqueues; not a full hypervisor/VM test",
    }, indent=2) + "\n")
    (linux / "lib/fh_irqfd_test.c").write_text(generated)
    makefile = linux / "lib/Makefile"
    line = "obj-y += fh_irqfd_test.o"
    if line not in makefile.read_text().splitlines():
        with makefile.open("a") as stream:
            stream.write("\n" + line + "\n")
    eventfd = linux / "fs/eventfd.c"
    # Test-only factory: calls the actual eventfd implementation and installs
    # its real file in the init task. Never part of the production adapter.
    if "int fh_irqfd_test_create(" not in eventfd.read_text():
        with eventfd.open("a") as stream:
            stream.write("\nint fh_irqfd_test_create(unsigned int count, int flags);\n"
                         "int fh_irqfd_test_create(unsigned int count, int flags)\n"
                         "{\n\treturn do_eventfd(count, flags);\n}\n")
    print("Installed real-kernel irqfd component harness in disposable tree")


if __name__ == "__main__":
    main()
