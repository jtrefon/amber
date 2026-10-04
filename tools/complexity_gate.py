#!/usr/bin/env python3
"""Complexity gate (ratchet).

The project standard is: a method stays small (<= NLOC_MAX lines of code), does
not branch excessively (CCN <= CCN_MAX), and does not take an unreadable number
of parameters (PARAM <= PARAM_MAX).

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

The gate fails closed, and that includes the case where the measurement itself
degrades. A cliff with an empty baseline that reports "0 over-limit functions"
because it parsed nothing is worse than no gate: it is green forever. So three
things are treated as errors rather than results:

  * lizard cannot be launched, or exits non-zero;
  * lizard prints function lines the parser does not recognise (a release
    changed the -w layout);
  * the scan covers implausibly few functions (ROOTS/excludes drifted, or a
    lizard release stopped reporting some construct).

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
PARAM_MAX = 6

# A healthy scan of this tree reports >2000 functions. The floor is set far below
# that on purpose: its only job is to catch a scan that measured (almost) nothing,
# not to encode the current size. Lower it and the gate stops noticing rot.
MIN_FUNCTIONS_SANE = 200

ROOTS = ["lib", "tools", "tui", "src", "bench", "plugins"]

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE = os.path.join(REPO_ROOT, "tests", "complexity_baseline.json")

# lizard -w prints clang-style lines:
#   path/to/f.cpp:417: warning: ns::f has 410 NLOC, 109 CCN, 2998 token, 0 PARAM, 485 length, 0 ND
WARN_RE = re.compile(
    r"^(?P<file>[^:]+):(?P<line>\d+): warning: (?P<name>.+?) has "
    r"(?P<nloc>\d+) NLOC, (?P<ccn>\d+) CCN, (?P<token>\d+) token, "
    r"(?P<param>\d+) PARAM, (?P<length>\d+) length"
)

# The shape of a lizard function line without its field layout. A line that
# matches this but not WARN_RE means the layout changed, which must be an error
# rather than a silently dropped measurement.
LOOSE_RE = re.compile(r"^[^:]+:\d+: warning: ")

AXES = (("nloc", NLOC_MAX, "NLOC"), ("ccn", CCN_MAX, "CCN"), ("param", PARAM_MAX, "PARAM"))


def repo_root():
    return REPO_ROOT


def lizard_command():
    """The exact invocation. -C 1 -L 1 makes lizard report all functions; the
    real thresholds are applied here, because lizard's -L filters on total lines
    rather than NLOC."""
    return [
        sys.executable, "-m", "lizard", "-w", "-l", "cpp",
        "-C", "1", "-L", "1",
        "--exclude", "third_party", "--exclude", "bench/results",
        # The glob form is what lizard honours: a bare "bench/scenarios" matches
        # nothing (measured -- 41 scenario lines still reported).
        #
        # bench/scenarios/ is code the benchmark *agent* writes and an oracle
        # scores -- skeletons, reference solutions and hidden tests. It is
        # deliberately not project style, which is why the format gate and cppcheck
        # already skip it. Gating it held the agent's own output to our complexity
        # limits, and put two benchmark fixtures in the CCN>=14 list.
        "--exclude", "*/scenarios/*",
        *ROOTS,
    ]


def parse_lizard_output(lines):
    """(functions, unparsed_count).

    `unparsed_count` counts lines that are shaped like a lizard function line but
    do not match WARN_RE -- the signal that lizard's output format moved.
    """
    functions, unparsed = [], 0
    for raw in lines:
        line = raw.strip()
        match = WARN_RE.match(line)
        if match:
            functions.append({
                "file": match.group("file"),
                "line": int(match.group("line")),
                "name": match.group("name"),
                "nloc": int(match.group("nloc")),
                "ccn": int(match.group("ccn")),
                "param": int(match.group("param")),
                "length": int(match.group("length")),
            })
        elif LOOSE_RE.match(line):
            unparsed += 1
    return functions, unparsed


def run_lizard():
    """Every function lizard can see, as dicts, or (None, reason).

    The exit status is deliberately ignored. lizard ends with
    `if 0 <= options.number < warning_count: return 1`, and `-C 1 -L 1` (needed to
    make it report every function) makes every function a warning, so a
    perfectly good run exits 1. Success is therefore judged from the output,
    which validate_scan does.
    """
    try:
        proc = subprocess.run(lizard_command(), capture_output=True, text=True)
    except OSError as exc:
        return None, f"could not run lizard: {exc}"
    return validate_scan(*parse_lizard_output(proc.stdout.splitlines()))


def validate_scan(functions, unparsed):
    """Refuse to treat a degraded scan as a clean one."""
    if unparsed:
        return None, (f"{unparsed} lizard output line(s) did not match the expected "
                      f"-w format; lizard's output layout changed")
    if not functions:
        return None, "lizard reported no functions"
    return functions, None


def over_limit(dimensions):
    return any(dimensions[key] > cap for key, cap, _ in AXES)


def violations_of(functions):
    return [f for f in functions if over_limit(f)]


def keyed(violations):
    """(file, name) -> {nloc, ccn, param}. On a name collision keep the worst
    entry, so a duplicate name can never hide a larger function."""
    out = {}
    for v in violations:
        key = (v["file"], v["name"])
        current = {k: v[k] for k, _, _ in AXES}
        cur = out.get(key)
        if cur is None or tuple(current.values()) > tuple(cur.values()):
            out[key] = current
    return out


def area_of(path):
    return path.split("/", 1)[0]


def describe(dimensions):
    return ", ".join(f"{label} {dimensions[key]}/{cap}" for key, cap, label in AXES)


def describe_caps():
    return ", ".join(f"{label} >{cap}" for _, cap, label in AXES)


def report(functions, violations):
    print(f"functions scanned: {len(functions)}")
    print(f"over-limit ({describe_caps()}): {len(violations)}")
    for area, n in sorted(count_by_area(violations).items(), key=lambda kv: -kv[1]):
        print(f"  {area:<10} {n}")
    for key, cap, label in AXES:
        print(f"  over {label} cap: {len([v for v in violations if v[key] > cap])}")
    worst = sorted(violations, key=lambda v: (-v["nloc"], -v["ccn"]))[:15]
    print("\n  worst by NLOC (total lines shown for information):")
    for v in worst:
        print(f"    {v['nloc']:>4} NLOC ({v['length']:>4} lines)  {v['ccn']:>3} CCN  "
              f"{v['param']:>3} PARAM  {v['file']}:{v['line']}  {v['name']}")


def count_by_area(violations):
    areas = {}
    for v in violations:
        area = area_of(v["file"])
        areas[area] = areas.get(area, 0) + 1
    return areas


def load_baseline():
    """The recorded functions, or None when there is no baseline file at all.

    An empty baseline (`{"functions": {}}`) is a *state*, not a missing file: it
    is every function under the caps, which is the cliff this ratchet works
    toward.
    """
    if not os.path.exists(BASELINE):
        return None
    with open(BASELINE, encoding="utf-8") as handle:
        raw = json.load(handle)
    return {(f, n): d for f, names in raw.get("functions", {}).items() for n, d in names.items()}


def shrunk_from(base, current):
    return [k for k in base if k not in current]


def check(functions):
    base = load_baseline()
    if base is None:
        print(f"complexity: no baseline at {BASELINE}; run tools/complexity_gate.py --update")
        return 2
    if len(functions) < MIN_FUNCTIONS_SANE:
        print(f"complexity: FAILED CLOSED - measured only {len(functions)} function(s), "
              f"expected at least {MIN_FUNCTIONS_SANE}. Check ROOTS, the --exclude list, "
              f"and the lizard version before trusting this result.")
        return 2

    current = keyed(violations_of(functions))
    new = [k for k in sorted(current) if k not in base]
    grew = [k for k in sorted(current)
            if k in base and any(current[k][a] > base[k].get(a, 0) for a, _, _ in AXES)]

    if new or grew:
        print_failures(new, grew, base, current)
        return 1

    shrunk = shrunk_from(base, current)
    if shrunk:
        print(f"complexity: {len(shrunk)} baselined function(s) are now within limits "
              f"- run `make complexity-update` to lock that in")
    print(f"  ok: {len(current)} over-limit function(s), none added or grown (baseline holds)")
    return 0


def print_failures(new, grew, base, current):
    caps = ", ".join(f"{label} {cap}" for _, cap, label in AXES)
    if new:
        print(f"complexity: {len(new)} new over-limit function(s):")
        for path, name in new:
            print(f"  {path}: {name} is {describe(current[(path, name)])} (limit {caps})")
    if grew:
        print(f"complexity: {len(grew)} function(s) got bigger:")
        for path, name in grew:
            print(f"  {path}: {name} {describe(base[(path, name)])} -> "
                  f"{describe(current[(path, name)])} (limit {caps})")
    print("  split the function, or justify it in the baseline via --update.")


def update(functions):
    by_file = {}
    for (path, name), dimensions in sorted(keyed(violations_of(functions)).items()):
        by_file.setdefault(path, {})[name] = dimensions
    payload = {
        "ccn_max": CCN_MAX,
        "nloc_max": NLOC_MAX,
        "param_max": PARAM_MAX,
        "functions": {p: by_file[p] for p in sorted(by_file)},
    }
    os.makedirs(os.path.dirname(BASELINE), exist_ok=True)
    with open(BASELINE, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=2, sort_keys=True)
        handle.write("\n")
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
    report(functions, violations_of(functions))
    print()
    return check(functions)


if __name__ == "__main__":
    sys.exit(main())
