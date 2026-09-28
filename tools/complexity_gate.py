#!/usr/bin/env python3
"""Complexity gate (ratchet).

The project standard is: a method stays small (<= LEN_MAX lines) and does not
branch excessively (CCN <= CCN_MAX). lizard measures both. This turns that into
a *ratchet* rather than a cliff: tests/complexity_baseline.json records the
accepted count of over-limit functions per file, so existing debt cannot grow
and a new file must be clean.

The gate fails closed: if lizard cannot run, the gate fails rather than
reporting success.

Usage:
  tools/complexity_gate.py --report    # list the violations (informational)
  tools/complexity_gate.py --check     # gate against the baseline (CI)
  tools/complexity_gate.py --update    # rewrite the baseline after fixing
"""

import argparse
import json
import os
import re
import subprocess
import sys

CCN_MAX = 15
LEN_MAX = 50
ROOTS = ["lib", "tools", "tui", "src", "bench", "plugins"]
BASELINE = os.path.join("tests", "complexity_baseline.json")

# lizard -w prints clang-style lines:
#   path/to/f.cpp:417: warning: ns::f has 410 NLOC, 109 CCN, 2998 token, 0 PARAM, 485 length, 0 ND
WARN_RE = re.compile(
    r"^(?P<file>[^:]+):(?P<line>\d+): warning: (?P<name>.+?) has "
    r"(?P<nloc>\d+) NLOC, (?P<ccn>\d+) CCN, (?P<token>\d+) token, "
    r"(?P<param>\d+) PARAM, (?P<length>\d+) length"
)


def run_lizard():
    """Return (violations, error). violations is a list of dicts."""
    cmd = [
        sys.executable, "-m", "lizard", "-w", "-l", "cpp",
        "--CCN", str(CCN_MAX), "-L", str(LEN_MAX),
        "--exclude", "third_party", "--exclude", "bench/results",
        *ROOTS,
    ]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True)
    except OSError as exc:
        return None, f"could not run lizard: {exc}"
    if proc.returncode != 0 and not proc.stdout.strip():
        return None, f"lizard failed ({proc.returncode}): {proc.stderr.strip()[:200]}"
    out = []
    for raw in proc.stdout.splitlines():
        m = WARN_RE.match(raw.strip())
        if not m:
            continue
        out.append({
            "file": m.group("file"),
            "line": int(m.group("line")),
            "name": m.group("name"),
            "nloc": int(m.group("nloc")),
            "ccn": int(m.group("ccn")),
            "length": int(m.group("length")),
        })
    return out, None


def counts_by_file(violations):
    counts = {}
    for v in violations:
        counts[v["file"]] = counts.get(v["file"], 0) + 1
    return counts


def area_of(path):
    return path.split("/", 1)[0]


def report(violations):
    by_area = {}
    for v in violations:
        by_area[area_of(v["file"])] = by_area.get(area_of(v["file"]), 0) + 1
    print(f"over-limit functions (CCN > {CCN_MAX} or length > {LEN_MAX}): {len(violations)}")
    for area, n in sorted(by_area.items(), key=lambda kv: -kv[1]):
        print(f"  {area:<10} {n}")
    dense = [v for v in violations if v["ccn"] > CCN_MAX and v["length"] > LEN_MAX]
    print(f"  of which dense AND long: {len(dense)}")
    print(f"  dense only (CCN > {CCN_MAX}): {len([v for v in violations if v['ccn'] > CCN_MAX and v['length'] <= LEN_MAX])}")
    print(f"  long only (length > {LEN_MAX}): {len([v for v in violations if v['ccn'] <= CCN_MAX and v['length'] > LEN_MAX])}")
    worst = sorted(violations, key=lambda v: (-v["length"], -v["ccn"]))[:15]
    print("\n  worst by length:")
    for v in worst:
        print(f"    {v['length']:>5} len  {v['ccn']:>4} CCN  {v['file']}:{v['line']}  {v['name']}")


def check(violations):
    if not os.path.exists(BASELINE):
        print(f"complexity: no baseline at {BASELINE}; run tools/complexity_gate.py --update")
        return 1
    with open(BASELINE) as fh:
        baseline = json.load(fh).get("files", {})
    current = counts_by_file(violations)
    regressions = []
    for path, n in sorted(current.items()):
        allowed = baseline.get(path, 0)
        if n > allowed:
            regressions.append((path, allowed, n))
    if regressions:
        print(f"complexity: {len(regressions)} file(s) gained over-limit functions:")
        for path, allowed, n in regressions:
            print(f"  {path}: {allowed} -> {n} (limit {LEN_MAX} lines / CCN {CCN_MAX})")
        print("  fix the function, or justify it in the baseline via --update.")
        return 1
    total = sum(current.values())
    print(f"  ok: {total} over-limit function(s), none added (baseline holds)")
    return 0


def update(violations):
    payload = {
        "ccn_max": CCN_MAX,
        "len_max": LEN_MAX,
        "files": dict(sorted(counts_by_file(violations).items())),
    }
    os.makedirs(os.path.dirname(BASELINE), exist_ok=True)
    with open(BASELINE, "w") as fh:
        json.dump(payload, fh, indent=2, sort_keys=True)
        fh.write("\n")
    print(f"complexity: baseline written to {BASELINE} ({sum(payload['files'].values())} violations)")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true", help="gate against the baseline")
    ap.add_argument("--report", action="store_true", help="print the violations")
    ap.add_argument("--update", action="store_true", help="rewrite the baseline")
    args = ap.parse_args()

    violations, err = run_lizard()
    if err:
        print(f"complexity: FAILED CLOSED - {err}", file=sys.stderr)
        print("complexity: install it with 'pip install lizard'", file=sys.stderr)
        return 2

    if args.update:
        return update(violations)
    if args.report:
        report(violations)
        return 0
    report(violations)
    print()
    return check(violations)


if __name__ == "__main__":
    sys.exit(main())
