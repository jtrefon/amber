#!/usr/bin/env python3
"""Lint-findings ratchet.

`make lint` used to be a cliff: any finding failed it, so a check that already
had findings could never be switched on (CX1). This applies the same ratchet the
complexity gate uses — tests/lint_baseline.json records how many findings each
(file, check) pair has, and the gate fails only when a count grows. Fixing
findings lowers the counts with --update, so the baseline can only shrink.

A count ratchet (not a line-by-line diff) is deliberate: it survives edits that
move code around, which a line-numbered baseline would not.

Usage:
  tools/lint_baseline.py --check   # gate (CI): no new findings
  tools/lint_baseline.py --update  # rewrite the baseline after fixing
  tools/lint_baseline.py --report  # print the current counts

Reads clang-tidy output on stdin, one finding per line:
  <path>:<line>:<col>: warning: <message> [<check>]
"""

import argparse
import collections
import json
import os
import re
import sys

BASELINE = os.path.join("tests", "lint_baseline.json")

# clang-tidy's finding line. The check name is the trailing [bracket].
FINDING = re.compile(r"^(?P<path>[^:]+):\d+:\d+: \w+: .*\[(?P<check>[-\w.]+)\]\s*$")


def repo_root():
    """The directory holding tests/, i.e. the repo root regardless of cwd."""
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.dirname(here)


def relative(path):
    """A finding path relative to the repo root, so the baseline is portable."""
    root = repo_root()
    absolute = path if os.path.isabs(path) else os.path.join(os.getcwd(), path)
    try:
        return os.path.relpath(os.path.realpath(absolute), os.path.realpath(root))
    except ValueError:  # different drive (Windows): keep it as written
        return path


def parse(text):
    """Count findings by (file, check). Unparsed lines are ignored, since
    clang-tidy also prints notes and 'N warnings generated' summaries."""
    counts = collections.defaultdict(lambda: collections.Counter())
    for line in text.splitlines():
        match = FINDING.match(line.strip())
        if match:
            counts[relative(match.group("path"))][match.group("check")] += 1
    return {path: dict(checks) for path, checks in sorted(counts.items())}


def total(counts):
    return sum(sum(checks.values()) for checks in counts.values())


def load_baseline():
    if not os.path.exists(BASELINE):
        return {}
    with open(BASELINE, encoding="utf-8") as handle:
        return json.load(handle).get("files", {})


def check(counts):
    """Fail only when a (file, check) count grew; report the shrunk ones."""
    baseline = load_baseline()
    if not baseline:
        print(f"lint-baseline: no baseline at {BASELINE}; run --update")
        return 2

    grew = []
    for path, checks in sorted(counts.items()):
        for name, count in sorted(checks.items()):
            was = baseline.get(path, {}).get(name, 0)
            if count > was:
                grew.append(f"  {path}: {name} {was} -> {count}")

    shrank = []
    for path, checks in sorted(baseline.items()):
        for name, was in sorted(checks.items()):
            count = counts.get(path, {}).get(name, 0)
            if count < was:
                shrank.append(f"  {path}: {name} {was} -> {count}")

    if grew:
        print("lint-baseline: new findings (ratchet)")
        print("\n".join(grew))
        return 1
    if shrank:
        print("lint-baseline: findings fixed — run `make lint-baseline-update`")
        print("\n".join(shrank))
    print(f"lint-baseline: {total(counts)} finding(s), none added (baseline holds)")
    return 0


def update(counts):
    with open(BASELINE, "w", encoding="utf-8") as handle:
        json.dump(
            {
                "_comment": (
                    "Known clang-tidy findings, counted per (file, check). The gate "
                    "fails only when a count grows, so this file can only shrink. "
                    "Regenerate with `make lint-baseline-update`."
                ),
                "files": counts,
            },
            handle,
            indent=2,
            sort_keys=True,
        )
        handle.write("\n")
    print(f"lint-baseline: baseline written to {BASELINE} ({total(counts)} findings)")
    return 0


def report(counts):
    for path, checks in sorted(counts.items(), key=lambda kv: -sum(kv[1].values())):
        for name, count in sorted(checks.items(), key=lambda kv: -kv[1]):
            print(f"{count:4d}  {path}  {name}")
    print(f"total: {total(counts)} finding(s)")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--check", action="store_true", help="gate against the baseline")
    group.add_argument("--update", action="store_true", help="rewrite the baseline")
    group.add_argument("--report", action="store_true", help="print the counts")
    args = parser.parse_args()

    counts = parse(sys.stdin.read())
    if args.check:
        return check(counts)
    if args.update:
        return update(counts)
    return report(counts)


if __name__ == "__main__":
    sys.exit(main())
