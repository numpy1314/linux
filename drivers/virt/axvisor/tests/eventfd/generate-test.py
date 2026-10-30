#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Install exact adapter eventfd callbacks in a disposable Linux test tree."""

import argparse
import hashlib
import json
import re
from pathlib import Path


def extract_function(text, name):
    pattern = rf"(?m)^(?:static )?(?:u64|ssize_t|int|void) {name}\("
    matches = list(re.finditer(pattern, text))
    if len(matches) != 1:
        raise ValueError("expected one function: " + name)
    start = matches[0].start()
    end = re.search(r"(?m)^}", text[start:])
    if end is None:
        raise ValueError("missing function end: " + name)
    return text[start:start + end.end()]


def extract_struct(text, name):
    pattern = rf"(?ms)^struct {name} \{{.*?^}};"
    matches = re.findall(pattern, text)
    if len(matches) != 1:
        raise ValueError("expected one struct: " + name)
    return matches[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--linux-source", type=Path, required=True)
    parser.add_argument("--runtime-source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    linux = args.linux_source.resolve()
    source = args.runtime_source.resolve()
    output = args.output.resolve()
    eventfd_source = linux / "fs/eventfd.c"
    if not eventfd_source.is_file() or not (linux / "Makefile").is_file():
        parser.error("--linux-source must be a disposable Linux 6.12 source tree")
    original_eventfd = eventfd_source.read_text()
    if "static int do_eventfd(unsigned int count, int flags)" not in original_eventfd:
        parser.error("unsupported eventfd creation API; expected Linux 6.12 do_eventfd")
    raw = source.read_bytes()
    runtime = raw.decode()
    pieces = [extract_struct(runtime, "axvisor_linux_control_fd_ref")]
    for name in ("axvisor_linux_control_get_fd_ref",
                 "axvisor_linux_control_write_fd_ref",
                 "axvisor_linux_control_read_fd_ref",
                 "axvisor_linux_control_release_fd_ref"):
        pieces.append(extract_function(runtime, name))
    pieces.append(extract_struct(runtime, "axvisor_task_start"))
    pieces.append(extract_function(runtime, "axvisor_linux_task_main"))
    pieces.append(extract_function(runtime, "axvisor_linux_join_task"))
    callbacks = "\n\n".join(pieces) + "\n"
    template = Path(__file__).with_name("fh_eventfd_test.c.in").read_text()
    marker = "/* EXTRACTED_ADAPTER_CALLBACKS */"
    if template.count(marker) != 1:
        parser.error("invalid test template")
    generated = template.replace(marker, callbacks)
    include = '#include "fh_eventfd_test.inc"'
    base_eventfd = original_eventfd.replace("\n" + include + "\n", "\n").rstrip() + "\n"
    output.mkdir(parents=True, exist_ok=True)
    (output / "callbacks-exact.c").write_text(callbacks)
    (output / "fh_eventfd_test.inc").write_text(generated)
    provenance = {
        "runtime_filename": source.name,
        "runtime_sha256": hashlib.sha256(raw).hexdigest(),
        "callbacks_sha256": hashlib.sha256(callbacks.encode()).hexdigest(),
        "generated_test_sha256": hashlib.sha256(generated.encode()).hexdigest(),
        "eventfd_source_without_harness_sha256": hashlib.sha256(
            base_eventfd.encode()).hexdigest(),
        "scope": "real Linux eventfd wait/cancel/reference regression; no guest execution",
    }
    (output / "source-provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
    (linux / "fs/fh_eventfd_test.inc").write_text(generated)
    if include not in original_eventfd.splitlines():
        eventfd_source.write_text(original_eventfd.rstrip() + "\n" + include + "\n")
    print("Installed real-kernel eventfd regression in disposable fs/eventfd.c")
    print("RUNTIME_SHA256=" + provenance["runtime_sha256"])
    print("CALLBACKS_SHA256=" + provenance["callbacks_sha256"])


if __name__ == "__main__":
    main()
