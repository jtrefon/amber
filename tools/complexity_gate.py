#!/usr/bin/env python3
"""Complexity gate (ratchet).

The project standard is: a method stays small (<= NLOC_MAX lines of code) and
does not branch excessively (CCN <= CCN_MAX).

Size is measured in **NLOC** (non-comment, non-blank lines), not total physical
lines. That is deliberate: a function is not too big because it is documented.
Gating on total lines would create pressure to delete explanatory comments to
pass the gate -- an incentive that makes a codebase worse, not better. lizard
reports both; `length` is shown in --report for information only.

This is a *ratchet*, not a cliff: tests/complexity_baseline.json records each
accepted over-limit function by name with its measured size, so existing debt
cannot grow and a new function must be clean. The baseline is keyed by
(file, function), so a file cannot swap one over-limit function for another and
pass -- the recorded function itself may not get bigger.

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
NLOC_MAX = 40
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
    """Every function lizard can see, as dicts. (-C 1 -L 1 makes lizard report
    all of them; the real thresholds are applied here, because lizard's -L
    filters on total lines, not NLOC.)"""
    cmd = [
        sys.executable, "-m", "lizard", "-w", "-l", "cpp",
        "-C", "1", "-L", "1",
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


def violations_of(functions):
    return [f for f in functions if f["ccn"] > CCN_MAX or f["nloc"] > NLOC_MAX]


def keyed(violations):
    """(file, name) -> {nloc, ccn}. On a name collision keep the worst entry,
    so a duplicate name can never hide a larger function."""
    out = {}
    for v in violations:
        key = (v["file"], v["name"])
        cur = out.get(key)
        if cur is None or (v["nloc"], v["ccn"]) > (cur["nloc"], cur["ccn"]):
            out[key] = {"nloc": v["nloc"], "ccn": v["ccn"]}
    return out


def area_of(path):
    return path.split("/", 1)[0]


def report(functions, violations):
    by_area = {}
    for v in violations:
        by_area[area_of(v["file"])] = by_area.get(area_of(v["file"]), 0) + 1
    print(f"functions scanned: {len(functions)}")
    print(f"over-limit (CCN > {CCN_MAX} or NLOC > {NLOC_MAX}): {len(violations)}")
    for area, n in sorted(by_area.items(), key=lambda kv: -kv[1]):
        print(f"  {area:<10} {n}")
    dense = [v for v in violations if v["ccn"] > CCN_MAX and v["nloc"] > NLOC_MAX]
    print(f"  of which dense AND long: {len(dense)}")
    print(f"  dense only (CCN > {CCN_MAX}): {len([v for v in violations if v['ccn'] > CCN_MAX and v['nloc'] <= NLOC_MAX])}")
    print(f"  long only (NLOC > {NLOC_MAX}): {len([v for v in violations if v['ccn'] <= CCN_MAX and v['nloc'] > NLOC_MAX])}")
    worst = sorted(violations, key=lambda v: (-v["nloc"], -v["ccn"]))[:15]
    print("\n  worst by NLOC (total lines shown for information):")
    for v in worst:
        print(f"    {v['nloc']:>4} NLOC ({v['length']:>4} lines)  {v['ccn']:>3} CCN  "
              f"{v['file']}:{v['line']}  {v['name']}")


def load_baseline():
    if not os.path.exists(BASELINE):
        return None
    with open(BASELINE) as fh:
        raw = json.load(fh)
    return {(f, n): d for f, names in raw.get("functions", {}).items() for n, d in names.items()}


def check(functions):
    base = load_baseline()
    if base is None:
        print(f"complexity: no baseline at {BASELINE}; run tools/complexity_gate.py --update")
        return 1
    current = keyed(violations_of(functions))

    grew, new = [], []
    for key, cur in sorted(current.items()):
        was = base.get(key)
        if was is None:
            new.append((key, cur))
        elif cur["nloc"] > was["nloc"] or cur["ccn"] > was["ccn"]:
            grew.append((key, was, cur))

    if grew or new:
        if new:
            print(f"complexity: {len(new)} new over-limit function(s):")
            for (path, name), cur in new:
                print(f"  {path}: {name} is {cur['nloc']} NLOC / CCN {cur['ccn']} "
                      f"(limit {NLOC_MAX} / {CCN_MAX})")
        if grew:
            print(f"complexity: {len(grew)} function(s) got bigger:")
            for (path, name), was, cur in grew:
                print(f"  {path}: {name} {was['nloc']}/{was['ccn']} -> "
                      f"{cur['nloc']}/{cur['ccn']} (limit {NLOC_MAX} / {CCN_MAX})")
        print("  split the function, or justify it in the baseline via --update.")
        return 1

    shrunk = [k for k in base if k not in current]
    if shrunk:
        print(f"complexity: {len(shrunk)} baselined function(s) are now within limits "
              f"- run `make complexity-update` to lock that in")
    print(f"  ok: {len(current)} over-limit function(s), none added or grown (baseline holds)")
    return 0


def update(functions):
    functions_by_file = {}
    for (path, name), dims in sorted(keyed(violations_of(functions)).items()):
        functions_by_file.setdefault(path, {})[name] = dims
    payload = {
        "ccn_max": CCN_MAX,
        "nloc_max": NLOC_MAX,
        "functions": {p: functions_by_file[p] for p in sorted(functions_by_file)},
    }
    os.makedirs(os.path.dirname(BASELINE), exist_ok=True)
    with open(BASELINE, "w") as fh:
        json.dump(payload, fh, indent=2, sort_keys=True)
        fh.write("\n")
    print(f"complexity: baseline written to {BASELINE} "
          f"({len(keyed(violations_of(functions)))} functions)")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true", help="gate against the baseline")
    ap.add_argument("--report", action="store_true", help="print the violations")
    ap.add_argument("--update", action="store_true", help="rewrite the baseline")
    args = ap.parse_args()

    functions, err = run_lizard()
    if err:
        print(f"complexity: FAILED CLOSED - {err}", file=sys.stderr)
        print("complexity: install it with 'pip install lizard'", file=sys.stderr)
        return 2

    if args.update:
        return update(functions)
    if args.report:
        report(functions, violations_of(functions))
        return 0
    report(functions, violations_of(functions))
    print()
    return check(functions)


if __name__ == "__main__":
    sys.exit(main())
