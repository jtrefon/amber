#!/usr/bin/env python3
"""Patch-coverage gate: new code must arrive covered.

CI already enforces a **project** floor — `gcovr --fail-under-line 80` over
`lib/`, `tools/` and `plugins/`. That is a real gate, but a project total is a
blunt instrument for the rule the project actually documents
(PULL_REQUEST_TEMPLATE.md, AGENTS.md): *"new code paths must have ≥80% line
coverage"*. A total stays satisfied while a large block of untested code lands, as
long as the total does not move — which is exactly the case the documented rule
exists to catch.

Delegating that to Codecov does not work here. The upload is rejected —
`Upload queued for processing failed: {"message":"Repository not found"}` — because
the Codecov GitHub App is not installed on this repository, and
`fail_ci_if_error: false` means the step still reports success. No Codecov status
check is ever created, so the `patch: target: 80%` in `.codecov.yml` has never
actually been evaluated. Measuring the diff here makes the rule real, keeps it
working with no third-party service in the path, and fails closed.

**Definition.** For every line the diff *adds*, was it executed by the test suite?
A line the diff touches that the report does not mention counts as **uncovered**:
absent data is not evidence of a test, and assuming otherwise is the fail-open
direction.

**Format.** Cobertura XML (`gcovr --xml-pretty`, which the coverage job already
produces as `coverage.xml`), because it carries per-line hit counts. gcovr's
`--json-summary` does **not**: its entries have no `lines` key at all, so a JSON
input here would measure nothing and -- correctly -- fail closed, which is a
confusing way to learn the format is wrong.

Usage:
  tools/coverage_gate.py --report coverage.xml --diff-base origin/main
  tools/coverage_gate.py --report coverage.xml --changed-lines changed.json
"""

import argparse
import json
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ElementTree

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The documented bar. Matches `gcovr --fail-under-line` in the coverage job.
COVERAGE_MIN = 80

HUNK = re.compile(r"^@@ -\d+(?:,\d+)? \+(?P<start>\d+)(?:,(?P<count>\d+))? @@")

# Only C++ is instrumented, so only C++ has a coverage number. A PR that edits
# ci.yml, a prompt or a Markdown file adds lines that no test can ever execute;
# counting them as uncovered would make the gate fail on documentation, which is
# both wrong and the fastest way to get a gate switched off.
INSTRUMENTED = (".cpp", ".c", ".cc", ".h", ".hpp", ".hxx")


def is_instrumented(path):
    return path.endswith(INSTRUMENTED)


class PatchCoverage:
    """Coverage of the lines a diff adds. `total` is None when there is nothing
    to measure, which is a pass, not a division by zero."""

    def __init__(self, total=None, covered=0, uncovered=None):
        self.total = total
        self.covered = covered
        self.uncovered = uncovered if uncovered is not None else []

    @property
    def covered_lines(self):
        return None if self.total is None else self.covered

    @property
    def percent(self):
        if not self.total:
            return None
        return 100.0 * self.covered / self.total

    @property
    def passes(self):
        return self.percent is None or self.percent >= COVERAGE_MIN


def skipped_paths(changed):
    """Changed files the gate ignored because they are not instrumented. Reported
    so an ignored file is a decision on the record rather than a silent gap."""
    return sorted(p for p in changed or {} if not is_instrumented(p))


def read_report(path):
    """{repo-relative path: {lineno: hits}} from Cobertura XML, or None when it
    cannot be read. None is a *result*, not an exception: the caller turns it into
    a fail-closed verdict."""
    try:
        root = ElementTree.parse(path).getroot()
    except (OSError, ElementTree.ParseError):
        return None
    hits = {}
    for entry in root.iter("class"):
        name = entry.get("filename")
        if not name:
            continue
        lines = {}
        for line in entry.iter("line"):
            number = line.get("number")
            if number is None:
                continue
            lines[int(number)] = int(line.get("hits", 0))
        if lines:
            hits[name] = lines
    return hits


def patch_coverage(hits, changed):
    """Coverage over the added lines. `changed` is {path: {lineno}}; None means
    no diff was supplied, which is a measurement failure, not a clean patch."""
    if hits is None or changed is None:
        return PatchCoverage()
    if not any(hits.values()):
        # No file in the report carries line data: the report measured nothing.
        return PatchCoverage()
    total, uncovered = 0, []
    for path, numbers in changed.items():
        if not is_instrumented(path):
            continue
        file_hits = hits.get(path, {})
        for number in sorted(numbers):
            total += 1
            if file_hits.get(number, 0) <= 0:
                uncovered.append((path, number))
    return PatchCoverage(total=total, covered=total - len(uncovered), uncovered=uncovered)


def check(result, changed=None):
    """Fail closed: a result with nothing to measure is an error, not a pass.
    The one exception is a diff that genuinely adds no lines (docs, whitespace),
    which is a legitimate pass and is signalled by changed being an empty dict."""
    if changed is None:
        print("coverage: FAILED CLOSED - no diff was supplied, so there is no "
              "changed-line set and the patch cannot be measured. Reporting a number "
              "here would be a guess.")
        return 2
    if result.total is None:
        print("coverage: FAILED CLOSED - the coverage report is missing or contains no "
              "line data, so patch coverage cannot be computed.")
        return 2
    if result.total == 0:
        print("coverage: no changed executable lines in the diff; nothing to gate "
              f"(the {COVERAGE_MIN}% rule applies to code, not to docs)")
        return 0
    verdict = "ok" if result.passes else "BELOW the bar"
    print(f"coverage: patch line coverage {result.percent:.1f}% "
          f"({result.covered}/{result.total}) - {verdict} "
          f"(bar: >= {COVERAGE_MIN}%)")
    if result.uncovered:
        shown = ", ".join(f"{p}:{n}" for p, n in result.uncovered[:15])
        more = "" if len(result.uncovered) <= 15 else f" (+{len(result.uncovered) - 15} more)"
        print(f"  uncovered added lines: {shown}{more}")
    return 0 if result.passes else 1


def changed_lines_from_diff(lines):
    """{path: {added line numbers}} from a unified diff."""
    changed, path, number = {}, None, 0
    for raw in lines:
        line = raw.rstrip("\n")
        if line.startswith("+++ "):
            target = line[4:].strip()
            path = None if target.startswith("/dev/null") else target[2:] \
                if target.startswith("b/") else target
            continue
        if line.startswith("--- ") or line.startswith("diff --git"):
            if line.startswith("diff --git"):
                path = None
            continue
        match = HUNK.match(line)
        if match:
            number = int(match.group("start"))
            continue
        if path is None or not line:
            continue
        if line.startswith("+"):
            changed.setdefault(path, set()).add(number)
            number += 1
        elif line.startswith("-"):
            continue
        elif line.startswith("\\"):  # "\ No newline at end of file"
            continue
        else:
            number += 1
    return {p: v for p, v in changed.items() if v}


def diff_against(base, path="."):
    """(diff lines, error). Empty output with an error means we must not gate."""
    try:
        proc = subprocess.run(["git", "-C", path, "diff", "--unified=0", base],
                              capture_output=True, text=True)
    except OSError as exc:
        return None, f"could not run git: {exc}"
    if proc.returncode != 0:
        return None, (f"git diff against {base} failed: {proc.stderr.strip()[:200]}. "
                      f"Is the base fetched? (actions/checkout with fetch-depth: 0)")
    return proc.stdout.splitlines(), None


def resolve_changed(args):
    """(changed, error) from either --changed-lines or --diff-base."""
    if args.changed_lines:
        try:
            with open(args.changed_lines, encoding="utf-8") as handle:
                raw = json.load(handle)
        except (OSError, ValueError) as exc:
            return None, f"could not read {args.changed_lines}: {exc}"
        return {p: set(v) for p, v in raw.items()}, None
    if args.diff_base:
        lines, err = diff_against(args.diff_base)
        if err:
            return None, err
        return changed_lines_from_diff(lines), None
    return None, "neither --changed-lines nor --diff-base was given"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--report", required=True, help="gcovr JSON summary")
    ap.add_argument("--diff-base", help="git ref to diff against, e.g. origin/main")
    ap.add_argument("--changed-lines", help="JSON {path: [lines]} instead of a diff")
    ap.add_argument("--markdown", help="write a one-line summary here (for the report)")
    args = ap.parse_args()

    changed, err = resolve_changed(args)
    if err:
        print(f"coverage: FAILED CLOSED - {err}", file=sys.stderr)
        return 2
    result = patch_coverage(read_report(args.report), changed)
    rc = check(result, changed)
    if args.markdown and result.percent is not None:
        write_markdown(args.markdown, result)
    return rc


def write_markdown(path, result):
    with open(path, "a", encoding="utf-8") as handle:
        handle.write(f"| coverage (patch) | {'pass' if result.passes else 'fail'} | "
                     f"{result.percent:.1f}% of {result.total} added line(s) "
                     f"(bar {COVERAGE_MIN}%) |\n")


if __name__ == "__main__":
    sys.exit(main())