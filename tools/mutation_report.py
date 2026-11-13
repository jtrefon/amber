#!/usr/bin/env python3
"""Summarise a Mull mutation-testing report.

Mull writes a SQLite database with one row per mutant. The runner's console
score treats a timeout as a kill, which hides a mutant the suite could not
finish; the advisory report keeps the three outcomes separate and prints a
conservative score (killed / total) next to Mull's own.

The tool is deliberately small because the mutation job is advisory: it reads a
report that already exists, never runs Mull itself, and fails closed when the
schema or the measurement is unusable.

Usage:
  tools/mutation_report.py --report mutation.sqlite
  tools/mutation_report.py --report mutation.sqlite --require-mutants \\
      --markdown mutation.md
"""

import argparse
import os
import sqlite3
import sys

KILLED, SURVIVED, TIMEOUT = 1, 2, 3
STATUS = {KILLED: "killed", SURVIVED: "survived", TIMEOUT: "timeout"}
REQUIRED_COLUMNS = ("mutator", "filename", "line_number", "execution_status",
                    "mutation_replacement")


def read_report(path, allow_missing=False):
    """[(mutator, file, line, status, replacement)] or an error string.

    `allow_missing` is for the advisory caller: mull-runner writes no SQLite
    file when the diff produced no mutants, which is a legitimate empty result
    (a comment-only change). The caller has already checked the runner's exit
    status, so a missing file is not a crash.
    """
    if allow_missing and not os.path.exists(path):
        return [], None
    try:
        connection = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    except sqlite3.Error as exc:
        return None, f"cannot open {path}: {exc}"
    try:
        rows = connection.execute(
            "select mutator, filename, line_number, execution_status, "
            "mutation_replacement from mutant").fetchall()
    except sqlite3.Error as exc:
        return None, f"cannot read the mutant table in {path}: {exc}"
    finally:
        connection.close()
    return rows, None


def summarise(rows, require_mutants=True):
    """Score and survivor list, or an error string when the report is unusable."""
    counts = {KILLED: 0, SURVIVED: 0, TIMEOUT: 0}
    survivors = []
    for mutator, filename, line, status, replacement in rows:
        if status not in counts:
            return None, (f"unknown mutant execution_status {status!r}; Mull's "
                          f"schema changed and this report cannot be trusted")
        counts[status] += 1
        if status == SURVIVED:
            survivors.append({"file": filename, "line": line, "mutator": mutator,
                              "replacement": replacement})
    total = sum(counts.values())
    if require_mutants and total == 0:
        return None, ("the report contains no mutants; the changed files were not "
                      "instrumented")
    result = {
        "total": total,
        "counts": {STATUS[key]: value for key, value in counts.items()},
        "survivors": sorted(survivors, key=lambda item: (item["file"], item["line"])),
    }
    result["conservative_score"] = 100.0 * counts[KILLED] / total if total else 0.0
    result["mull_score"] = 100.0 * (counts[KILLED] + counts[TIMEOUT]) / total if total else 0.0
    return result, None


def markdown(result, base):
    counts = result["counts"]
    lines = [
        "## Mutation testing (advisory)",
        "",
        f"Base: `{base}`. Mull mutates only the changed production lines; the",
        "job reports the score and the survivors, and does not fail the build.",
        "",
        "| metric | value |",
        "|---|---:|",
        f"| mutants | {result['total']} |",
        f"| killed | {counts['killed']} |",
        f"| timed out | {counts['timeout']} |",
        f"| survived | {counts['survived']} |",
        f"| conservative score (killed / total) | {result['conservative_score']:.1f}% |",
        f"| Mull score (timeouts count as kills) | {result['mull_score']:.1f}% |",
    ]
    if result["survivors"]:
        lines += ["", "### Surviving mutants", ""]
        for survivor in result["survivors"]:
            lines.append(f"- `{survivor['file']}:{survivor['line']}` "
                         f"`{survivor['mutator']}` -> `{survivor['replacement']}`")
    else:
        lines += ["", "No surviving mutants."]
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--report", required=True, help="Mull SQLite report")
    ap.add_argument("--markdown", help="write the summary here")
    ap.add_argument("--base", default="origin/main", help="base ref named in the report")
    ap.add_argument("--require-mutants", action="store_true",
                    help="fail when the report is empty (a changed tree that was not measured)")
    ap.add_argument("--allow-missing", action="store_true",
                    help="treat a missing report as an empty one (no mutants found)")
    ap.add_argument("--min-score", type=float, default=None,
                    help="optional conservative-score floor; advisory runs omit it")
    args = ap.parse_args()

    rows, error = read_report(args.report, allow_missing=args.allow_missing)
    if rows is None:
        print(f"mutation-report: FAILED CLOSED - {error}", file=sys.stderr)
        return 2
    result, error = summarise(rows, require_mutants=args.require_mutants)
    if result is None:
        print(f"mutation-report: FAILED CLOSED - {error}", file=sys.stderr)
        return 2
    text = markdown(result, args.base)
    if args.markdown:
        with open(args.markdown, "w", encoding="utf-8") as handle:
            handle.write(text)
    print(text, end="")
    if args.min_score is not None and result["conservative_score"] < args.min_score:
        print(f"mutation-report: {result['conservative_score']:.1f}% is below the "
              f"{args.min_score:.1f}% floor", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
