#!/usr/bin/env python3
"""Class-size ratchet.

AGENTS.md asks that a class/struct definition stay under 200 lines, and notes
the limit is "enforced in review, not by the compiler". This makes it enforced
like every other size limit in the project: a ratchet over a baseline, so the
gate fails when a type GROWS rather than when any type is large.

Same shape as tools/complexity_gate.py and tools/lint_baseline.py: a cliff would
fail on the types that are already over, which is why the rule stayed in review
instead of becoming a gate.

Size is measured in **code lines** -- neither blank nor comment-only -- because
the cap is about how much a type *declares*. Gating on total lines would make
deleting the comments that explain a public interface the cheapest way to pass.

The gate fails closed. `measure()` is a brace scanner, not a parser, so it can
lose sync; when it does, it must say so rather than report a smaller number:
  * a scan that measured implausibly few types is an error, not a pass;
  * a type whose braces never balance is reported as unscannable, not skipped.

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

# A healthy scan finds hundreds of types. Like the complexity gate's function
# floor, this exists only to catch a scan that measured nothing.
MIN_TYPES_SANE = 50

BASELINE = os.path.join("tests", "class_size_baseline.json")

# A type definition, not a forward declaration (`class Foo;`).
TYPE_START = re.compile(r"^\s*(class|struct)\s+(\w+)\s*(?::|\{)")

SCAN_DIRS = ("include", "lib", "tui", "src", "tools", "bench", "plugins")

# Vendored code is not ours to split, and its size is not our debt. The
# complexity gate excludes third_party for the same reason.
SKIP_PARTS = ("nlohmann", "third_party", "vendor")


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def strip_noise(text):
    """Remove comments and string/char literals, keeping every other character.

    A brace inside any of them is punctuation in *data*, not structure. Counting
    it is how a large class measures as small: bench/probe.cpp carries SSE
    payloads whose escaped JSON has unbalanced braces inside string literals.

    Line structure is preserved exactly -- every newline in the input produces
    one newline in the output -- so the result can be split into lines and
    indexed alongside the original.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        two = text[i:i + 2]
        if two == "//":
            end = text.find("\n", i)
            if end < 0:
                break
            i = end
            continue
        if two == "/*":
            start = i
            end = text.find("*/", i + 2)
            i = n if end < 0 else end + 2
            out.append(keep_newlines(text[start:i]))
            continue
        char = text[i]
        if char == '"' or char == "'":
            start = i
            i = skip_literal(text, i)
            out.append(keep_newlines(text[start:i]))
            continue
        out.append(char)
        i += 1
    return "".join(out)


def keep_newlines(span):
    """`span` with every non-newline character blanked, so line numbering and
    column positions survive while the content cannot be mistaken for code."""
    return "".join("\n" if c == "\n" else " " for c in span)


def skip_literal(text, start):
    """Index just past the literal beginning at `start`, honouring escapes.

    A C++ raw string (R"delim(...)delim") is handled too: its body is arbitrary
    text and routinely contains braces and quotes.
    """
    quote = text[start]
    if text[start:start + 2] in ('R"', "R'"):
        return skip_raw_string(text, start)
    i = start + 1
    while i < len(text):
        if text[i] == "\\":
            i += 2
            continue
        if text[i] == quote:
            return i + 1
        if text[i] == "\n":
            return i  # an unterminated literal ends at the line break
        i += 1
    return i


def skip_raw_string(text, start):
    """Index just past a raw string literal beginning at `start` (R"delim( )."""
    open_paren = text.find("(", start)
    if open_paren < 0:
        return len(text)
    delim = text[start + 2:open_paren]
    closer = ")" + delim + '"'
    end = text.find(closer, open_paren)
    return len(text) if end < 0 else end + len(closer)


def code_lines(lines):
    """Lines that are neither blank nor comment-only."""
    count = 0
    in_block = False
    for line in lines:
        s = line.strip()
        if in_block:
            if "*/" in s:
                in_block = False
            continue
        if not s or s.startswith("//"):
            continue
        if s.startswith("/*"):
            if "*/" not in s:
                in_block = True
            continue
        count += 1
    return count


def extent(lines, start):
    """(end_index, balanced) for the type whose `class|struct` keyword is at
    `start`. `balanced` is False when the braces never close, which means the
    scanner lost sync -- the caller must not treat that as a small type."""
    depth = 0
    for j in range(start, len(lines)):
        depth += lines[j].count("{") - lines[j].count("}")
        if depth == 0 and j > start:
            return j, True
    return len(lines) - 1, False


def measure(path):
    """Every type definition in one file, as (code_lines, name).

    Types whose braces never balance are omitted: they cannot be measured
    reliably, and `unscannable()` reports them separately.
    """
    try:
        with open(path, encoding="utf-8", errors="ignore") as handle:
            raw = handle.read()
    except OSError:
        return []

    lines = raw.split("\n")
    structural = strip_noise(raw).split("\n")
    found = []
    for i, line in enumerate(structural):
        match = TYPE_START.match(line)
        if not match:
            continue
        end, balanced = extent(structural, i)
        if balanced:
            found.append((code_lines(lines[i:end + 1]), match.group(2)))
    return found


def unscannable(path):
    """Type names in `path` whose braces never balance."""
    try:
        with open(path, encoding="utf-8", errors="ignore") as handle:
            raw = handle.read()
    except OSError:
        return []
    structural = strip_noise(raw).split("\n")
    names = []
    for i, line in enumerate(structural):
        match = TYPE_START.match(line)
        if match and not extent(structural, i)[1]:
            names.append(match.group(2))
    return names


def source_files():
    root = repo_root()
    for directory in SCAN_DIRS:
        for base, _, files in os.walk(os.path.join(root, directory)):
            for name in sorted(files):
                if not name.endswith((".h", ".hpp", ".cpp")):
                    continue
                path = os.path.join(base, name)
                rel = os.path.relpath(path, root)
                if any(part in rel.split(os.sep) for part in SKIP_PARTS):
                    continue
                yield rel, path


def scan():
    """(types over the cap keyed by relative path, types measured, unscannable)."""
    over, measured, broken = {}, 0, {}
    for rel, path in source_files():
        types = measure(path)
        measured += len(types)
        big = {name: lines for lines, name in types if lines > MAX_LINES}
        if big:
            over[rel] = big
        stuck = unscannable(path)
        if stuck:
            broken[rel] = stuck
    return {k: over[k] for k in sorted(over)}, measured, broken


def total(over):
    return sum(len(v) for v in over.values())


def load_baseline():
    """The recorded types, or None when there is no baseline file at all.

    An empty baseline (`{"types": {}}`) is a state, not a missing file: it means
    every type is under the cap, which is the cliff the ratchet works toward.
    """
    path = os.path.join(repo_root(), BASELINE)
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8") as handle:
        return json.load(handle).get("types", {})


def check(over, measured, broken):
    """`measured` and `broken` are required, not optional: a caller that forgets
    to pass the scan's liveness signals must not get a silent pass."""
    baseline = load_baseline()
    if baseline is None:
        print(f"class-size: no baseline at {BASELINE}; run --update")
        return 2
    if broken:
        return report_unscannable(broken)
    if measured < MIN_TYPES_SANE:
        print(f"class-size: FAILED CLOSED - measured only {measured} type(s), expected at "
              f"least {MIN_TYPES_SANE}. Check SCAN_DIRS and SKIP_PARTS before trusting this.")
        return 2

    grew = []
    for path, types in sorted(over.items()):
        for name, lines in sorted(types.items()):
            was = baseline.get(path, {}).get(name)
            if was is None:
                grew.append(f"  {path}: {name} is {lines} lines (new, cap {MAX_LINES})")
            elif lines > was:
                grew.append(f"  {path}: {name} {was} -> {lines} lines")

    shrank = shrink_notices(baseline, over)
    if grew:
        print("class-size: new or grown types (ratchet)")
        print("\n".join(grew))
        return 1
    if shrank:
        print("class-size: types split — run `make class-size-update`")
        print("\n".join(shrank))
    print(f"class-size: {total(over)} type(s) over {MAX_LINES}, none added (baseline holds)")
    return 0


def shrink_notices(baseline, over):
    notices = []
    for path, types in sorted(baseline.items()):
        for name, was in sorted(types.items()):
            lines = over.get(path, {}).get(name)
            if lines is None:
                notices.append(f"  {path}: {name} {was} -> under {MAX_LINES}")
            elif lines < was:
                notices.append(f"  {path}: {name} {was} -> {lines}")
    return notices


def report_unscannable(broken):
    print("class-size: FAILED CLOSED - a type's braces never balance, so its size cannot "
          "be measured. A brace-aware scanner that cannot see the end of a type is worse "
          "than no gate, because it reports a smaller number.")
    for path, names in sorted(broken.items()):
        print(f"  {path}: {', '.join(sorted(names))}")
    return 2


def update(over):
    path = os.path.join(repo_root(), BASELINE)
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(
            {
                "_comment": (
                    f"Types over {MAX_LINES} code lines (comments and blanks excluded), by "
                    "file. The gate fails only when a type grows or a new one appears, so "
                    "this file can only shrink. Regenerate with `make class-size-update`."
                ),
                "max_lines": MAX_LINES,
                "types": over,
            },
            handle,
            indent=2,
            sort_keys=True,
        )
        handle.write("\n")
    print(f"class-size: baseline written to {path} ({total(over)} types over {MAX_LINES})")
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

    if args.report:
        return report(scan()[0])
    over, measured, broken = scan()
    if args.update:
        return update(over)
    return check(over, measured, broken)


if __name__ == "__main__":
    sys.exit(main())
