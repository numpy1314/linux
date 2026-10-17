#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Check actual kernel assertion results, not merely QEMU's exit status."""
import argparse
import json
import re
from pathlib import Path


def verify(legacy, fixed, arch, preempt_count):
    expected = 55 if arch == "arm64" else 52
    expected += 5 if preempt_count else 0
    requirements = {
        "legacy_result": "FH_TEST RESULT mode=legacy checks=3 failures=0" in legacy,
        "legacy_order_warning": bool(re.search(r"WARNING:.*mm/page_alloc\.c:", legacy)),
        "fixed_result": f"FH_TEST RESULT mode=fixed checks={expected} failures=0" in fixed,
        "fixed_no_kernel_fault": not re.search(
            r"WARNING:|BUG:|Oops:|Kernel panic|rcu:.*stall|sleeping function called|FH_TEST FAIL", fixed),
        "pinned_array_forced_vmalloc": fixed.count("FH_TEST PASS pinned_array_4MiB_plus8_vmalloc") == 2,
        "pinned_array_atomic_free": "FH_TEST PASS pinned_array_atomic_free_returned" in fixed,
        "full_buffer_integrity": "FH_TEST PASS full_buffer_integrity_size_4194320_align_8" in fixed,
    }
    if arch == "arm64":
        requirements["raw_daif_with_priority_masking"] = (
            "FH_TEST RAW_DAIF linux_irqs_disabled=0 bridge_atomic=1" in fixed)
    if preempt_count:
        requirements["preempt_count_config"] = bool(re.search(r"preempt_count=1 debug_atomic_sleep=1", fixed))
        requirements["preempt_disabled_process_context"] = (
            "FH_TEST PASS preempt_disabled_is_process_context_with_irqs_enabled" in fixed)
        requirements["preempt_disabled_allocation"] = (
            "FH_TEST PASS preempt_disabled_oversize_rejected_without_warning" in fixed)
    else:
        requirements["preempt_count_disabled_reported"] = (
            "FH_TEST SKIP preempt_disabled CONFIG_PREEMPT_COUNT=n" in fixed)
    return {
        "scope": "Linux adapter allocator only; no Firecracker or guest lifecycle claim",
        "architecture": arch,
        "expected_fixed_assertions": expected,
        "debug_atomic_sleep_variant": preempt_count,
        "checks": requirements,
        "passed": all(requirements.values()),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--legacy-log", required=True, type=Path)
    parser.add_argument("--fixed-log", required=True, type=Path)
    parser.add_argument("--arch", required=True, choices=("arm64", "x86_64"))
    parser.add_argument("--preempt-count", action="store_true",
                        help="require the additional DEBUG_ATOMIC_SLEEP/PREEMPT_COUNT variant")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = verify(args.legacy_log.read_text(errors="replace"),
                    args.fixed_log.read_text(errors="replace"), args.arch, args.preempt_count)
    encoded = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(encoded)
    print(encoded, end="")
    raise SystemExit(0 if result["passed"] else 1)


if __name__ == "__main__":
    main()
