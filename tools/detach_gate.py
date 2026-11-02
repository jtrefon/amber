#!/usr/bin/env python3
"""No new `std::thread::detach()` call sites (ratchet).

A detached thread outlives the object that spawned it, so its captures can be
freed while it still runs and there is nothing to join at shutdown. AGENTS.md
bans new detach usage; the tree already has five call sites, so this is a
ratchet rather than a cliff:

  * a file that gains a detach fails, and a new file with one fails;
  * a reduction is fine and can be recorded with `--update` (the baseline only
    shrinks).

It fails closed on a scan that read almost nothing, like the other gates.

Usage:
  tools/detach_gate.py --check
  tools/detach_gate.py --update
  tools/detach_gate.py --report
"""

import argparse
import json
import os
import re
import sys

import cpp_source
import hygiene_gate

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE = os.path.join("tests", "detach_baseline.json")

DETACH = re.compile(r"\.detach\s*\(")

# A healthy scan reads hundreds of files; a scan that read almost nothing must
# not report a clean ratchet.
MIN_FILES_SANE = 100


def count_detaches(source):
    """Detach calls in code, not in comments or string literals."""
    structural = cpp_source.strip_noise(source)
    return len(DETACH.findall(structural))


def current_counts():
    """{relative path: count} for files with at least one detach."""
    counts = {}
    for rel, path in hygiene_gate.source_files():
        source, _ = cpp_source.read(path)
        if source is None:
            continue
        count = count_detaches(source)
        if count:
            counts[rel] = count
    return counts


def load_baseline(path=None):
    with open(path or os.path.join(REPO_ROOT, BASELINE), encoding="utf-8") as handle:
        return json.load(handle).get("files", {})


def regressions(current, baseline):
    """(path, current, allowed) for every file above its recorded count."""
    bad = []
    for path, count in sorted(current.items()):
        allowed = baseline.get(path, 0)
        if count > allowed:
            bad.append((path, count, allowed))
    return bad


def scanned_files():
    return sum(1 for _ in hygiene_gate.source_files())


def check(current, baseline, scanned):
    if scanned < MIN_FILES_SANE:
        print(f"detach: FAILED CLOSED - scanned only {scanned} file(s), expected at least "
              f"{MIN_FILES_SANE}.", file=sys.stderr)
        return 2
    bad = regressions(current, baseline)
    if bad:
        print("detach: new std::thread::detach() call site(s):")
        for path, count, allowed in bad:
            print(f"  {path}: {count} (baseline {allowed})")
        print("  Join the thread, or keep the work on the owning thread. An existing "
              "detach is recorded debt; a new one is a shutdown/use-after-free risk.")
        return 1
    total = sum(current.values())
    print(f"detach: {scanned} file(s) scanned, {total} detach(es) "
          f"(baseline {sum(baseline.values())}) - no new sites")
    return 0


def report(current, baseline):
    total = sum(current.values())
    print(f"detach call sites: {total} in {len(current)} file(s)")
    for path, count in sorted(current.items()):
        allowed = baseline.get(path, 0)
        delta = "" if count == allowed else f"  (baseline {allowed})"
        print(f"  {path}: {count}{delta}")
    removed = sorted(set(baseline) - set(current))
    if removed:
        print(f"  recorded sites no longer present: {', '.join(removed)}")
        print("  run `make detach-update` to shrink the baseline")


def save(current, path=None):
    target = path or os.path.join(REPO_ROOT, BASELINE)
    payload = {
        "_comment": "Existing std::thread::detach() call sites. The rule is no NEW detach; "
                    "this file can only shrink. Regenerate with `make detach-update`.",
        "files": dict(sorted(current.items())),
    }
    with open(target, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=2, sort_keys=True)
        handle.write("\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument("--check", action="store_true", help="fail on a new detach")
    group.add_argument("--update", action="store_true", help="record the current counts")
    group.add_argument("--report", action="store_true", help="print counts and baseline")
    args = ap.parse_args()

    scanned = scanned_files()
    current = current_counts()
    if args.update:
        if scanned < MIN_FILES_SANE:
            print(f"detach: refusing to update from a scan of {scanned} file(s)",
                  file=sys.stderr)
            return 2
        save(current)
        print(f"detach: baseline updated ({sum(current.values())} site(s))")
        return 0
    baseline = load_baseline()
    if args.report:
        report(current, baseline)
        return 0
    return check(current, baseline, scanned)


if __name__ == "__main__":
    sys.exit(main())
