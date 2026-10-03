#!/usr/bin/env python3
"""Lint-findings ratchet.

`make lint` used to be a cliff: any finding failed it, so a check that already
had findings could never be switched on (CX1). This applies the same ratchet the
complexity gate uses — tests/lint_baseline.json records how many findings each
(file, check) pair has, and the gate fails only when a count grows. Fixing
findings lowers the counts with --update, so the baseline can only shrink.

A count ratchet (not a line-by-line diff) is deliberate: it survives edits that
move code around, which a line-numbered baseline would not.

The gate fails closed. clang-tidy cannot tell us whether it ran — it exits
non-zero for ordinary findings too — so two explicit signals are required:

  * A compile error surfaces as a `[clang-diagnostic-error]` diagnostic, which is
    shaped exactly like a finding. If that were counted, `--update` would record
    "the tree does not compile" as a known finding and tolerate it forever. Hard
    errors are therefore never findings, and their presence fails the gate.
  * Zero findings is a legitimate state, indistinguishable by counting from a
    crashed run. The caller states how many translation units it asked clang-tidy
    to process (`--expect-tu`) and emits one `lint-tidy-tu:<path>` sentinel per
    unit; a shortfall means the gate measured nothing.

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

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE = os.path.join(REPO_ROOT, "tests", "lint_baseline.json")

FINDING = re.compile(r"^(?P<path>[^:]+):\d+:\d+: \w+: .*\[(?P<check>[-\w.]+)\]\s*$")

# A compiler diagnostic rather than a lint finding. Shape-compatible with
# FINDING, so it must be filtered before counting, never after.
HARD_ERROR = "clang-diagnostic-error"

# "Error while processing <file>." is clang-tidy's banner when a TU fails to
# compile. It carries no [check], so it would otherwise be silently dropped.
ERROR_BANNER = re.compile(r"^Error while processing .*\.$")

# Emitted by the Makefile once per translation unit handed to clang-tidy.
TU_SENTINEL = re.compile(r"^lint-tidy-tu:(?P<path>.+)$")


def repo_root():
    """The directory holding tests/, i.e. the repo root regardless of cwd."""
    return REPO_ROOT


def relative(path):
    """A finding path relative to the repo root, so the baseline is portable.

    The Makefile hands clang-tidy repo-relative paths, so a relative path is
    already repo-relative; resolving it against the cwd instead would make the
    recorded baseline depend on where the gate was invoked from.
    """
    root = os.path.realpath(repo_root())
    absolute = path if os.path.isabs(path) else os.path.join(root, path)
    try:
        return os.path.relpath(os.path.realpath(absolute), root)
    except ValueError:  # different drive (Windows): keep it as written
        return path


def classify(lines):
    """(counts, hard_errors, tus) from raw clang-tidy output.

    Unparsed lines are ignored, since clang-tidy also prints notes and
    'N warnings generated' summaries.
    """
    counts = collections.defaultdict(lambda: collections.Counter())
    hard_errors, tus = 0, 0
    for raw in lines:
        line = raw.strip()
        sentinel = TU_SENTINEL.match(line)
        if sentinel:
            tus += 1
            continue
        if ERROR_BANNER.match(line):
            hard_errors += 1
            continue
        match = FINDING.match(line)
        if not match:
            continue
        if match.group("check") == HARD_ERROR:
            hard_errors += 1
            continue
        counts[relative(match.group("path"))][match.group("check")] += 1
    return {path: dict(checks) for path, checks in sorted(counts.items())}, hard_errors, tus


def parse(text):
    """Counts by (file, check), excluding hard errors."""
    return classify(text.splitlines())[0]


def total(counts):
    return sum(sum(checks.values()) for checks in counts.values())


def load_baseline():
    """The recorded counts, or None when there is no baseline file at all.

    An empty baseline (`{"files": {}}`) is a *state*, not a missing file: it
    means every check is at zero, which is the hard cliff the ratchet is
    working toward. Only a genuinely absent file is an error.
    """
    if not os.path.exists(BASELINE):
        return None
    with open(BASELINE, encoding="utf-8") as handle:
        return json.load(handle).get("files", {})


def has_hard_error(counts):
    """Defence in depth: classify() already strips hard errors, but a baseline
    must never be able to carry one, whatever the caller passes."""
    return any(HARD_ERROR in checks for checks in counts.values())


def check(counts, errored=False, ran=None, expected=None):
    """Fail only when a (file, check) count grew; report the shrunk ones."""
    baseline = load_baseline()
    if baseline is None:
        print(f"lint-baseline: no baseline at {BASELINE}; run --update")
        return 2
    if errored or has_hard_error(counts):
        print("lint-baseline: FAILED CLOSED - clang-tidy reported a compile error "
              f"[{HARD_ERROR}]. A translation unit that does not compile cannot be "
              "analysed, so this run measured nothing. Fix the compile error.")
        return 2
    if expected is not None and (ran or 0) < expected:
        print(f"lint-baseline: FAILED CLOSED - clang-tidy processed {ran or 0} of "
              f"{expected} translation unit(s). Zero findings is only meaningful when "
              "the whole tree was analysed, so this result is discarded.")
        return 2

    grew = []
    for path, checks in sorted(counts.items()):
        for name, count in sorted(checks.items()):
            was = baseline.get(path, {}).get(name, 0)
            if count > was:
                grew.append(f"  {path}: {name} {was} -> {count}")

    shrank = shrink_notices(baseline, counts)
    if grew:
        print("lint-baseline: new findings (ratchet)")
        print("\n".join(grew))
        return 1
    if shrank:
        print("lint-baseline: findings fixed — run `make lint-baseline-update`")
        print("\n".join(shrank))
    print(f"lint-baseline: {total(counts)} finding(s), none added (baseline holds)")
    return 0


def shrink_notices(baseline, counts):
    notices = []
    for path, checks in sorted(baseline.items()):
        for name, was in sorted(checks.items()):
            count = counts.get(path, {}).get(name, 0)
            if count < was:
                notices.append(f"  {path}: {name} {was} -> {count}")
    return notices


def update(counts, hard_errors=0):
    """Rewrite the baseline. Refuses to record a compile error: a baseline that
    says "this file does not compile" would tolerate a broken tree forever."""
    if hard_errors or has_hard_error(counts):
        print(f"lint-baseline: refusing to update the baseline: "
              f"{hard_errors or 'a'} compile error(s) ([{HARD_ERROR}]). Recording them "
              f"would tolerate a tree that does not build.", file=sys.stderr)
        return 2
    with open(BASELINE, "w", encoding="utf-8") as handle:
        json.dump(
            {
                "_comment": (
                    "Known clang-tidy findings, counted per (file, check). The gate "
                    "fails only when a count grows, so this file can only shrink. "
                    "Compile errors are never recorded here: see the module docstring. "
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
    ap = argparse.ArgumentParser(description=__doc__)
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument("--check", action="store_true", help="gate against the baseline")
    group.add_argument("--update", action="store_true", help="rewrite the baseline")
    group.add_argument("--report", action="store_true", help="print the counts")
    ap.add_argument("--expect-tu", type=int, default=None,
                    help="how many translation units the caller handed to clang-tidy")
    args = ap.parse_args()

    counts, hard_errors, tus = classify(sys.stdin.read().splitlines())
    if args.update:
        return update(counts, hard_errors)
    if args.report:
        return report(counts)
    return check(counts, errored=hard_errors > 0, ran=tus, expected=args.expect_tu)


if __name__ == "__main__":
    sys.exit(main())
