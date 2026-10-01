#!/usr/bin/env python3
"""Class-size ratchet.

AGENTS.md asks that a class/struct definition stay under 200 lines, and notes
the limit is "enforced in review, not by the compiler". This makes it enforced
like every other size limit in the project: a ratchet over a baseline, so the
gate fails when a type GROWS rather than when any type is large.

Same shape as tools/complexity_gate.py and tools/lint_baseline.py: a cliff would
fail on the three types that are already over, which is why the rule stayed in
review instead of becoming a gate.

Usage:
  tools/class_size_gate.py --check    # gate (CI): no type grew past the cap
  tools/class_size_gate.py --update   # rewrite the baseline after splitting
  tools/class_size_gate.py --report   # print every type over the cap
"""

import argparse
import json
import os
import re
import sys

MAX_LINES = 200
BASELINE = os.path.join("tests", "class_size_baseline.json")

# A type definition, not a forward declaration (`class Foo;`).
TYPE_START = re.compile(r"^\s*(class|struct)\s+(\w+)\s*(?::|\{)")

SCAN_DIRS = ("include", "lib", "tui", "src", "tools", "bench", "plugins")

# Vendored code is not ours to split, and its size is not our debt. The
# complexity gate excludes third_party for the same reason.
SKIP_PARTS = ("nlohmann", "third_party", "vendor")


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def measure(path):
    """Every type definition in one file, as (lines, name)."""
    try:
        with open(path, encoding="utf-8", errors="ignore") as handle:
            lines = handle.read().split("\n")
    except OSError:
        return []

    found = []
    for i, line in enumerate(lines):
        match = TYPE_START.match(line)
        if not match:
            continue
        depth = 0
        for j in range(i, len(lines)):
            depth += lines[j].count("{") - lines[j].count("}")
            if depth == 0 and j > i:
                found.append((j - i + 1, match.group(2)))
                break
    return found


def scan():
    """Every type over the cap, keyed by relative path."""
    root = repo_root()
    over = {}
    for directory in SCAN_DIRS:
        for base, _, files in os.walk(os.path.join(root, directory)):
            for name in files:
                if not name.endswith((".h", ".hpp", ".cpp")):
                    continue
                path = os.path.join(base, name)
                rel = os.path.relpath(path, root)
                if any(part in rel.split(os.sep) for part in SKIP_PARTS):
                    continue
                big = {t: n for n, t in measure(path) if n > MAX_LINES}
                if big:
                    over[rel] = big
    return {k: over[k] for k in sorted(over)}


def total(over):
    return sum(len(v) for v in over.values())


def load_baseline():
    if not os.path.exists(os.path.join(repo_root(), BASELINE)):
        return {}
    with open(os.path.join(repo_root(), BASELINE), encoding="utf-8") as handle:
        return json.load(handle).get("types", {})


def check(over):
    baseline = load_baseline()
    if not baseline:
        print(f"class-size: no baseline at {BASELINE}; run --update")
        return 2

    grew = []
    for path, types in sorted(over.items()):
        for name, lines in sorted(types.items()):
            was = baseline.get(path, {}).get(name)
            if was is None:
                grew.append(f"  {path}: {name} is {lines} lines (new, cap {MAX_LINES})")
            elif lines > was:
                grew.append(f"  {path}: {name} {was} -> {lines} lines")

    shrank = []
    for path, types in sorted(baseline.items()):
        for name, was in sorted(types.items()):
            lines = over.get(path, {}).get(name)
            if lines is None:
                shrank.append(f"  {path}: {name} {was} -> under {MAX_LINES}")
            elif lines < was:
                shrank.append(f"  {path}: {name} {was} -> {lines}")

    if grew:
        print("class-size: new or grown types (ratchet)")
        print("\n".join(grew))
        return 1
    if shrank:
        print("class-size: types split — run `make class-size-update`")
        print("\n".join(shrank))
    print(f"class-size: {total(over)} type(s) over {MAX_LINES}, none added (baseline holds)")
    return 0


def update(over):
    path = os.path.join(repo_root(), BASELINE)
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(
            {
                "_comment": (
                    f"Types over {MAX_LINES} lines, by file. The gate fails only when a "
                    "type grows or a new one appears, so this file can only shrink. "
                    "Regenerate with `make class-size-update`."
                ),
                "max_lines": MAX_LINES,
                "types": over,
            },
            handle,
            indent=2,
            sort_keys=True,
        )
        handle.write("\n")
    print(f"class-size: baseline written to {BASELINE} ({total(over)} types over {MAX_LINES})")
    return 0


def report(over):
    for path, types in sorted(over.items(), key=lambda kv: -max(kv[1].values())):
        for name, lines in sorted(types.items(), key=lambda kv: -kv[1]):
            print(f"{lines:5d}  {path}  {name}")
    print(f"total: {total(over)} type(s) over {MAX_LINES}")
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--check", action="store_true", help="gate against the baseline")
    group.add_argument("--update", action="store_true", help="rewrite the baseline")
    group.add_argument("--report", action="store_true", help="print types over the cap")
    args = parser.parse_args()

    over = scan()
    if args.check:
        return check(over)
    if args.update:
        return update(over)
    return report(over)


if __name__ == "__main__":
    sys.exit(main())
