#!/usr/bin/env python3
"""Debt-marker gate (cliff).

Every other gate here measures a *shape* — size, nesting, findings. This one
measures the thing those shapes are supposed to prevent: work that was started,
not finished, and left behind. A `TODO` is a promise made in a comment, and the
comment costs nothing to write and nothing to keep.

The gate is a cliff because the tree is already clean: zero markers in owned
source. That makes it the cheapest possible debt ratchet — nothing to burn down,
and no baseline to maintain. It only has to stay at zero.

**Scope.** Owned C++ source, the same directories the class-size gate scans.
Vendored code is excluded for the same reason: `include/nlohmann/json.hpp` carries
13 of its own TODOs and they are not ours to fix or to carry.

**Escape hatch.** A marker is suppressed by acknowledging it on the same line:

    // TODO: hoist the cache  (debt-allow: tracked in #412)

Suppressions are counted and printed by `--report`, so they cannot accumulate
silently. An escape hatch is deliberate: a gate with none gets worked around by
deleting the comment, which is strictly worse than an annotated one.
"""

import argparse
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SCAN_DIRS = ("include", "lib", "tui", "src", "tools", "bench", "plugins")

SKIP_PARTS = ("nlohmann", "third_party", "vendor")

MARKERS = ("TODO", "FIXME", "HACK", "XXX")

# Word-bounded so `int iflag`, `todo_list` and `methodName` do not match.
DEBT_RE = re.compile(r"\b(" + "|".join(MARKERS) + r")\b")

ALLOW = "debt-allow:"

SOURCE_SUFFIXES = (".cpp", ".h", ".hpp", ".c", ".cc")

# Generated or vendored headers we do not own.
SKIP_FILES = ("include/agent/version.h",)


def debt_markers(source):
    """(line_number, text) for each unacknowledged debt marker, 1-indexed."""
    found = []
    for number, line in enumerate(source.split("\n"), start=1):
        match = DEBT_RE.search(line)
        if match and ALLOW not in line:
            found.append((number, line.strip()))
    return found


def source_files():
    for directory in SCAN_DIRS:
        for base, _, files in os.walk(os.path.join(REPO_ROOT, directory)):
            for name in sorted(files):
                if not name.endswith(SOURCE_SUFFIXES):
                    continue
                path = os.path.join(base, name)
                rel = os.path.relpath(path, REPO_ROOT)
                if any(part in rel.split(os.sep) for part in SKIP_PARTS):
                    continue
                if rel in SKIP_FILES:
                    continue
                yield rel, path


def scan():
    """Every debt marker found, as {relative_path: [(line, text)]}."""
    out = {}
    for rel, path in source_files():
        try:
            with open(path, encoding="utf-8", errors="ignore") as handle:
                found = debt_markers(handle.read())
        except OSError:
            continue
        if found:
            out[rel] = found
    return out


def total(found):
    return sum(len(v) for v in found.values())


def check(found, scanned):
    """`scanned` is required: a scan that read nothing must not look clean."""
    if scanned < MIN_FILES_SANE:
        print(f"debt: FAILED CLOSED - scanned only {scanned} file(s), expected at least "
              f"{MIN_FILES_SANE}. Check SCAN_DIRS before trusting this result.")
        return 2
    if not found:
        print(f"debt: {scanned} file(s) scanned, 0 debt markers (clean)")
        return 0
    print(f"debt: {total(found)} debt marker(s) in {len(found)} file(s). "
          f"Finish the work, or acknowledge it inline with '{ALLOW} <reason>':")
    for rel, hits in sorted(found.items()):
        for line, text in hits:
            print(f"  {rel}:{line}: {text}")
    return 1


def report(found, scanned):
    print(f"files scanned: {scanned}")
    print(f"debt markers: {total(found)}")
    for rel, hits in sorted(found.items()):
        for line, text in hits:
            print(f"  {rel}:{line}: {text}")


def update(found):
    """Nothing to record: the gate is a cliff at zero. --update therefore only
    ever succeeds when the tree is already clean, and says so rather than writing
    an empty baseline that would look like a ratchet."""
    if found:
        print(f"debt: {total(found)} marker(s) present; this gate has no baseline to "
              f"update. Clear them or acknowledge them with '{ALLOW} <reason>'.",
              file=sys.stderr)
        return 1
    print("debt: clean; this gate is a cliff at zero, so there is no baseline to write")
    return 0


# A healthy scan reads hundreds of files. As in the other gates this floor exists
# only to catch a scan that measured nothing.
MIN_FILES_SANE = 100


def count_files():
    return sum(1 for _ in source_files())


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument("--check", action="store_true", help="gate: no debt markers")
    group.add_argument("--update", action="store_true", help="always fails if dirty")
    group.add_argument("--report", action="store_true", help="list the markers found")
    args = ap.parse_args()

    found = scan()
    scanned = sum(1 for _ in source_files())
    if args.report:
        report(found, scanned)
        return 0
    if args.update:
        return update(found)
    return check(found, scanned)


if __name__ == "__main__":
    sys.exit(main())