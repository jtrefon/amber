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

**Definition.** For every *executable* line the diff adds, was it executed by the
test suite?

Which lines those are comes from the report, not from guesswork. Measured against
gcov's own output, gcovr's Cobertura XML behaves like this:

    executable and ran            -> <line hits="1">
    executable but never ran      -> <line hits="0">      <- uncovered
    comment, blank, closing brace -> no <line> at all    <- not executable

So a changed line the report does not mention is **not executable**, and demanding
coverage of it is wrong. An earlier version of this gate assumed the opposite --
that absent meant untested -- and consequently failed a PR whose only addition to a
`.cpp` was four lines of explanatory comment. The gate caught it on its own first
live run, which is the argument for having it.

A changed `.cpp` that is missing from the report *entirely* is a different matter:
it was compiled, so it must be there. That is a measurement gap, and it fails
closed rather than counting as zero.

**Carryover.** A line the diff *moved* rather than wrote is not new code, so it is
not held against the patch. This matters more than it sounds: the gate's first live
run failed a pure refactor -- 16 lines lifted out of an inline lambda into a named
method -- and reported every one as uncovered. A diff-based gate that punishes code
motion blocks exactly the refactoring this project is trying to encourage, and the
incentive it creates is to leave code badly organised rather than move it.

A moved line is identified by its text: if an added line's content already appeared
somewhere in the base revision of that file, it was carried over. The rule is
deliberately generous -- a genuinely new line that happens to be textually identical
to an existing one is also skipped. The asymmetry is on purpose: a false positive
blocks a merge over code that was never untested, which is the more expensive
mistake.

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
import re
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

# Translation units. If one of these is absent from the report it was compiled but
# not measured, which is a gap worth failing on. Headers are not translation units:
# a header with no inline definitions has no executable lines of its own and is
# routinely absent, so failing on that would block every header-touching PR.
TRANSLATION_UNITS = (".cpp", ".c", ".cc")

# Tests are excluded from the coverage report on purpose -- you do not measure
# coverage of the thing that measures coverage -- so a PR that adds a test would
# otherwise fail this gate with "absent from the report". Same exclusion the
# coverage job's gcovr filters already apply.
NOT_MEASURED_PREFIXES = ("tests/",)


def deliberately_not_measured(path, extra_patterns=()):
    """Whether the coverage report intentionally omits this source.

    tests/ is excluded because you do not measure coverage of the thing that measures
    coverage. The `main()` entry points are excluded by the coverage job's gcovr
    filters: they are process drivers with no surviving source of their own.

    Both matter for the same reason, and confusing the second with the first is a
    fail-open in the wrong direction: a changed .cpp that is absent from the report is
    normally a measurement gap worth failing on, so an *excluded* .cpp has to be
    recognised explicitly or every PR touching one is blocked for the wrong reason.
    The patterns are passed in by the workflow rather than hardcoded here, so the
    gcovr exclusion list stays the single source of truth.
    """
    if path.startswith(NOT_MEASURED_PREFIXES):
        return True
    return any(re.search(pat, path) for pat in extra_patterns)


def is_instrumented(path):
    """Whether a path has coverage at all.

    Non-C++ (ci.yml, Markdown, prompts) is skipped because no test can execute it;
    tests/ is skipped because the coverage report deliberately excludes it. Either
    way, a gate that fails on those gets switched off rather than fixed.
    """
    if deliberately_not_measured(path):
        return False
    return path.endswith(INSTRUMENTED)


def is_translation_unit(path):
    return path.endswith(TRANSLATION_UNITS)


class PatchCoverage:
    """Coverage of the lines a diff adds. `total` is None when there is nothing
    to measure, which is a pass, not a division by zero."""

    def __init__(self, total=None, covered=0, uncovered=None, carried=0):
        self.total = total
        self.covered = covered
        self.uncovered = uncovered if uncovered is not None else []
        # Added lines whose text already existed in the base revision: moved, not
        # written. Counted separately so the report can say so out loud.
        self.carried = carried
        # Changed sources the report does not contain at all. See the module
        # docstring: absent per line means "not executable", absent per file means
        # "not measured", and those are opposite conclusions.
        self.unmeasured = []

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


def patch_coverage(hits, changed, carried_over=None, absent_ok=()):
    """Coverage over the added lines.

    `changed` is {path: {lineno: text}}; None means no diff was supplied, which is
    a measurement failure, not a clean patch. `carried_over` is {path: set of
    normalised texts already present at the base revision}.
    """
    if hits is None or changed is None:
        return PatchCoverage()
    carried_over = carried_over or {}
    if not any(hits.values()):
        # No file in the report carries line data: the report measured nothing.
        return PatchCoverage()
    total, uncovered, unmeasured, carried = 0, [], [], 0
    for path, numbers in changed.items():
        if not is_instrumented(path) or deliberately_not_measured(path, absent_ok):
            continue
        if path not in hits:
            if is_translation_unit(path):
                # Compiled source missing from the report means the coverage job did
                # not measure it. A gap in the measurement, not a low score.
                unmeasured.append(path)
            continue
        file_hits = hits[path]
        known = carried_over.get(path, frozenset())
        for number in sorted(numbers):
            if number not in file_hits:
                continue  # not executable: comment, blank, brace
            total += 1
            if file_hits[number] <= 0:
                if normalise(numbers[number]) in known:
                    carried += 1   # moved here from elsewhere in the file
                    total -= 1
                    continue
                uncovered.append((path, number))
    result = PatchCoverage(total=total, covered=total - len(uncovered),
                           uncovered=uncovered, carried=carried)
    result.unmeasured = sorted(set(unmeasured))
    return result


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
    if result.unmeasured:
        print("coverage: FAILED CLOSED - these changed sources are absent from the coverage "
              "report. They were compiled, so they should have been measured; counting them "
              "as untested would be a guess in the other direction:")
        for path in result.unmeasured[:15]:
            print(f"  {path}")
        return 2
    if result.total == 0:
        # Say so when the reason was carryover rather than an empty diff: "nothing
        # to gate" reads like the diff was empty when in fact lines moved.
        if result.carried:
            print(f"coverage: no new executable lines - {result.carried} added line(s) "
                  f"already existed elsewhere in the file (moved code is not new code)")
            return 0
        print("coverage: no changed executable lines in the diff; nothing to gate "
              f"(the {COVERAGE_MIN}% rule applies to code, not to docs)")
        return 0
    verdict = "ok" if result.passes else "BELOW the bar"
    moved = f", {result.carried} carried over from elsewhere in the file" \
        if result.carried else ""
    print(f"coverage: patch line coverage {result.percent:.1f}% "
          f"({result.covered}/{result.total}{moved}) - {verdict} "
          f"(bar: >= {COVERAGE_MIN}%)")
    if result.uncovered:
        shown = ", ".join(f"{p}:{n}" for p, n in result.uncovered[:15])
        more = "" if len(result.uncovered) <= 15 else f" (+{len(result.uncovered) - 15} more)"
        print(f"  uncovered added lines: {shown}{more}")
    return 0 if result.passes else 1


def normalise(line):
    """Text for carryover comparison: whitespace collapsed, so a reindented move
    still counts as a move."""
    return " ".join(line.split())


def changed_lines_from_diff(lines):
    """{path: {added line number: added line text}} from a unified diff.

    The text is kept because carryover needs it: a line counts as moved if its
    content already existed in the base revision.
    """
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
            changed.setdefault(path, {})[number] = line[1:]
            number += 1
        elif line.startswith("-"):
            continue
        elif line.startswith("\\"):  # "\ No newline at end of file"
            continue
        else:
            number += 1
    return {p: v for p, v in changed.items() if v}


def base_lines(base, paths):
    """{path: set of normalised line texts} as they were at `base`.

    Read from git rather than reconstructed from the diff, so a line that moved
    from elsewhere in the file is recognised as well as one that moved up or down.
    """
    out = {}
    for path in paths:
        try:
            proc = subprocess.run(["git", "-C", REPO_ROOT, "show", f"{base}:{path}"],
                                  capture_output=True, text=True)
        except OSError:
            continue
        if proc.returncode == 0:
            out[path] = {normalise(l) for l in proc.stdout.split("\n")}
    return out


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
    ap.add_argument("--absent-ok", action="append", default=[], metavar="REGEX",
                    help="changed source matching this is excluded from the coverage "
                         "report on purpose; not a measurement gap (repeatable)")
    args = ap.parse_args()

    changed, err = resolve_changed(args)
    if err:
        print(f"coverage: FAILED CLOSED - {err}", file=sys.stderr)
        return 2
    carried_over = base_lines(args.diff_base, changed) if args.diff_base else {}
    result = patch_coverage(read_report(args.report), changed, carried_over,
                           tuple(args.absent_ok))
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