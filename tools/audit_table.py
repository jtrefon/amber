#!/usr/bin/env python3
"""Check the AGENTS.md architecture-audit table against the tree.

The table in AGENTS.md lists files with a line count. That count is a fact about the
tree, so it drifts the moment somebody edits a listed file -- and because every PR that
adds tests edits the test file, every such PR used to conflict on that one row with every
other open PR. Two or three in flight and the merge queue can never drain.

A row may therefore declare `-` instead of a number, meaning "exempt from the exact
count": the file is still listed (its size is part of the audit), but no PR has to touch
the row to change it. Exemption is a property of the FILE (EXEMPT below), not of the
text: a source file that writes `-` to dodge the check fails here, which is the shape
this gate exists to prevent. An exempt file that declares a number is still checked
against the tree.

Run from the repository root, or with `--selftest` (no filesystem access).
"""

import re
import sys
from pathlib import Path

# Files whose exact line count is not worth a per-PR edit. Keep this list to files the
# audit table itself describes as exempt from the rule it tracks; anything else keeps a
# number and is checked.
EXEMPT = {"tests/run_tests.cpp"}

# Rows the table must still carry. A row silently deleted is a file that stops being
# audited, which is the same failure as a wrong count.
REQUIRED = ("tests/run_tests.cpp", "lib/session.cpp", "tui/tui_render.cpp", "tui/tui_input.cpp")

ROW = re.compile(r"^\| `([^`]+)` \| ([^|]*) \|")
EXEMPT_CELL = "-"
NUMBER = re.compile(r"^\d+$")
TABLE_HEADER = "| File | Lines | Issue |"


def audit_rows(text):
    """The audit table's rows only.

    AGENTS.md has other pipe tables (the Context operations, the tool list), and their
    first column is a method name, not a path. Parsing only the table under its own
    header keeps those out of the count check instead of reporting them as malformed.
    """
    rows = {}
    in_table = False
    for line in text.splitlines():
        if line.startswith(TABLE_HEADER):
            in_table = True
            continue
        if not in_table:
            continue
        if not line.startswith("|"):
            break
        match = ROW.match(line)
        if match:
            rows[match.group(1)] = match.group(2).strip()
    return rows


def problems(text, count_of):
    """Every way the table disagrees with the tree. `count_of(path)` is the line count."""
    rows = audit_rows(text)

    out = []
    for path in REQUIRED:
        if path not in rows:
            out.append(f"{path}: listed in the audit table before, now missing")
    for path, cell in sorted(rows.items()):
        if cell == EXEMPT_CELL:
            if path not in EXEMPT:
                out.append(f"{path}: declares '{EXEMPT_CELL}' but is not an exempt file")
            continue
        if not NUMBER.match(cell):
            out.append(f"{path}: '{cell}' is neither a line count nor '{EXEMPT_CELL}'")
            continue
        actual = count_of(path)
        if actual is None:
            out.append(f"{path}: listed in the audit table but not in the tree")
        elif actual != int(cell):
            out.append(f"{path}: table says {cell} lines, tree has {actual}")
    return out


def _selftest():
    counts = {"tests/run_tests.cpp": 9400, "lib/session.cpp": 348,
              "tui/tui_render.cpp": 125, "tui/tui_input.cpp": 4066}
    table = (
        "| File | Lines | Issue |\n|------|------:|-------|\n"
        "| `tests/run_tests.cpp` | {test} | Test file |\n"
        "| `lib/session.cpp` | {session} | Resolved |\n"
        "| `tui/tui_render.cpp` | 125 | Exempt |\n"
        "| `tui/tui_input.cpp` | 4066 | Exempt |\n"
    )
    cases = [
        ("counts match", table.format(test=9400, session=348), counts, []),
        ("a count drifts", table.format(test=9306, session=348), counts,
         ["tests/run_tests.cpp: table says 9306 lines, tree has 9400"]),
        ("exempt row may say '-'", table.format(test=EXEMPT_CELL, session=348), counts, []),
        ("a source row may not say '-'", table.format(test=9400, session=EXEMPT_CELL), counts,
         [f"lib/session.cpp: declares '{EXEMPT_CELL}' but is not an exempt file"]),
        ("a cell that is neither", table.format(test=9400, session="many"), counts,
         ["lib/session.cpp: 'many' is neither a line count nor '-'"]),
        ("a listed file that is gone", table.format(test=9400, session=348),
         {k: v for k, v in counts.items() if k != "tui/tui_render.cpp"},
         ["tui/tui_render.cpp: listed in the audit table but not in the tree"]),
        ("a deleted row",
         "| File | Lines | Issue |\n"
         "| `tests/run_tests.cpp` | - | Test file |\n"
         "| `lib/session.cpp` | 348 | Resolved |\n"
         "| `tui/tui_render.cpp` | 125 | Exempt |\n",
         counts, ["tui/tui_input.cpp: listed in the audit table before, now missing"]),
    ]
    failures = 0
    for desc, text, table_counts, want in cases:
        got = problems(text, table_counts.get)
        if got != want:
            failures += 1
            print(f"  FAIL {desc}: want {want or 'pass'}, got {got or 'pass'}", file=sys.stderr)
        else:
            print(f"  ok   {desc}")
    if failures:
        print(f"audit_table: {failures} self-test case(s) failed", file=sys.stderr)
        return 1
    print(f"audit_table: {len(cases)} self-test cases passed")
    return 0


def _line_count(path):
    try:
        with open(path, "rb") as f:
            return sum(1 for _ in f)
    except OSError:
        return None


def main(argv):
    if "--selftest" in argv:
        return _selftest()
    try:
        text = Path("AGENTS.md").read_text()
    except OSError as exc:
        print(f"audit_table: FAILED CLOSED - cannot read AGENTS.md: {exc}", file=sys.stderr)
        return 2
    found = problems(text, _line_count)
    if found:
        print("audit_table: the AGENTS.md audit table disagrees with the tree:")
        for item in found:
            print(f"  {item}")
        return 1
    print("audit_table: audit table matches the tree")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
