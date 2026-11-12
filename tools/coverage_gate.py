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

**Branches.** A covered line can still have every decision untested: an `if`
whose body never runs has one covered condition and one uncovered condition, so
line coverage calls it done. gcovr's Cobertura output carries the aggregate on
each branch line as `condition-coverage="P% (covered/total)"`, and the gate reads
that attribute.

Only **decision lines** are counted. gcov instruments the exception/destructor
edges of every object construction as "jump" branches, and no test can reach
them; `gcovr --exclude-throw-branches --exclude-unreachable-branches` removes
some at report time, and the gate additionally classifies the source line
(`if`/`for`/`while`/`switch`/`case`/`catch`, `&&`/`||`/`?`, or a wrapped
condition continuation). Without that filter a new `std::string` or
`nlohmann::json` construction would add "uncovered branches" that are not
decisions at all, and the ratchet would fail on well-tested code.

Two rules, because neither sees what the other does:

- **Patch branch coverage** over added lines in `lib/`, `tools/` and `plugins/`
  (the same scope as the project line floor), at `BRANCH_COVERAGE_MIN` (50%).
  The measured tree sits at 47.6% branches, so 50% is a ratchet above the status
  quo rather than an aspiration. TUI, `src/` and `bench/` are printed per changed
  file but not enforced: the pty/e2e harness is what exercises them, and its
  branch reach is not yet good enough to make the number mean what it means in
  the core.
- **A per-file ratchet** in `tests/branch_baseline.json`, measured by the CI
  coverage job. Each gated file may not carry more uncovered branches than the
  baseline records; a file the baseline does not name starts at zero. This is
  what a percentage cannot do: one file can add ten covered branches and one
  untested branch and still clear any aggregate bar. Only a deliberate, committed
  update lowers the baseline (`--update-branch-baseline`,
  `make coverage-branch-baseline`).

Branch counts are compiler-generated, so the baseline is measured with the CI
toolchain (gcc 13 on ubuntu-24.04, gcovr 8.6). A local run with a different GCC
can disagree by a few conditions in a large file; run the coverage recipe with
`CXX=g++-13` for a number comparable to the baseline.

Usage:
  tools/coverage_gate.py --report coverage.xml --diff-base origin/main \
      --branch-baseline tests/branch_baseline.json
  tools/coverage_gate.py --report coverage.xml --changed-lines changed.json
  tools/coverage_gate.py --report coverage.xml \
      --update-branch-baseline tests/branch_baseline.json
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

# Branch coverage of the patch. Lower than the line bar on purpose: the measured
# core sits at 47.6% branches, and an 80% patch bar would have blocked the
# cache-gauge PR (60.9%) that was well tested. The per-file ratchet below is the
# part that makes "a new untested branch" fail, whatever the patch percentage.
BRANCH_COVERAGE_MIN = 50

# Branch coverage is gated for the same sources as the project line floor. The
# TUI and the bench harness are driven by pty/e2e processes whose branch reach is
# not comparable yet, so their numbers are reported and not enforced.
BRANCH_GATED_PREFIXES = ("lib/", "tools/", "plugins/")

# `condition-coverage="50% (1/2)"` -- gcovr's aggregate for one line. Its
# `<conditions>` children exist too, but their order is an implementation detail
# and the attribute is the number gcovr itself reports.
CONDITION = re.compile(r"^(\d+)% \((\d+)/(\d+)\)$")

# A branch is gated only when its source line is a decision. gcov instruments the
# exception/destructor edges of every object construction as "jump" branches, and
# no test can reach those; counting them would demand 100% coverage of C++
# plumbing. `--exclude-throw-branches` removes some at report time, but
# constructor edges survive it, so the gate classifies the line itself.
DECISION = re.compile(r"\b(if|for|while|switch|case|catch)\b|&&|\|\||\?")
# A condition that wraps continues on the next line when the line before it ends
# with an operator or an opening delimiter.
CONTINUATION = re.compile(r"(\|\||&&|\(|,|\?)\s*$")

# A ratchet on a coverage count inherits the measurement's noise. Two
# consecutive CI runs of the same commit moved `lib/dispatch.cpp` by 1 and
# `tools/bash_tool.cpp` by 2 uncovered decisions, from timing-dependent tests.
# A zero-growth rule would fail a clean tree; the slack absorbs that noise. The
# patch bar is still what catches a single new untested branch, because it
# measures the diff rather than a whole file.
BRANCH_RATCHET_SLACK = 3

BASELINE_COMMENT = (
    "Uncovered decision-branch counts per file, measured by the CI coverage job "
    "(gcovr 8.6, gcc 13 on ubuntu-24.04). Only lines that hold a decision count: "
    "gcov's constructor/exception edges are not code paths. The gate fails when a "
    "count grows beyond the measured run-to-run slack, so this file can only "
    "shrink. Regenerate with `make coverage-branch-baseline` after a deliberate "
    "reduction."
)

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


def is_branch_gated(path):
    """Whether a path's branches are held to the patch bar and the ratchet.

    The same source scope as the project line floor. A TUI branch is still
    measured and printed; it is not enforced, because the pty harness exercises
    fewer decisions than the unit suite reaches in the core.
    """
    return path.startswith(BRANCH_GATED_PREFIXES)


def read_source(path, root=REPO_ROOT):
    """{lineno: text} for a repo-relative source, or None when it cannot be read."""
    try:
        with open(os.path.join(root, path), encoding="utf-8", errors="replace") as handle:
            return {number: line.rstrip("\n") for number, line in enumerate(handle, start=1)}
    except OSError:
        return None


def load_sources(paths, root=REPO_ROOT):
    return {path: read_source(path, root) for path in paths}


def is_decision_line(lines, number):
    """Whether a reported branch line holds a decision, or continues one.

    `lines` is {lineno: text} or None. None means the source could not be read;
    counting the line is the conservative answer, because silently dropping a
    decision would hide it from both the bar and the ratchet.
    """
    if lines is None:
        return True
    if DECISION.search(lines.get(number, "")):
        return True
    for previous in range(number - 1, 0, -1):
        line = lines.get(previous, "").strip()
        if not line:
            continue
        return bool(CONTINUATION.search(line))
    return False


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
        # BranchCoverage when a branch report was supplied, else None. Separate
        # from the line totals so a caller that has no branch data keeps the old
        # line-only behaviour instead of silently passing the branch gate.
        self.branches = None

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


class BranchCoverage:
    """Branch coverage of the lines a diff adds.

    `total` covers the gated scope only (`lib/`, `tools/`, `plugins/`) and is
    None when no branch report was supplied. `by_file` carries every instrumented
    changed file, so a TUI number stays visible without being enforced.
    `report_empty` records a report that parsed but contains no branch data at
    all: that is a measurement gap, not a tree with zero decisions.
    """

    def __init__(self, total=None, covered=0, uncovered=None, carried=0,
                 by_file=None, report_empty=False):
        self.total = total
        self.covered = covered
        # (path, line, covered conditions, total conditions)
        self.uncovered = uncovered if uncovered is not None else []
        # Conditions on moved lines, reported but not held against the patch.
        self.carried = carried
        self.by_file = by_file if by_file is not None else {}
        self.report_empty = report_empty

    @property
    def percent(self):
        if not self.total:
            return None
        return 100.0 * self.covered / self.total

    @property
    def passes(self):
        return self.percent is None or self.percent >= BRANCH_COVERAGE_MIN


def skipped_paths(changed):
    """Changed files the gate ignored because they are not instrumented. Reported
    so an ignored file is a decision on the record rather than a silent gap."""
    return sorted(p for p in changed or {} if not is_instrumented(p))


def class_lines(entry):
    """The file-level `<lines>` block of a Cobertura class.

    Not `entry.iter("line")`: gcovr repeats every line inside each `<method>`
    block, so iterating the subtree double-counts, and a template instantiated
    into three methods appears six times. The class-level block is the file's own
    data; within it a template line can still appear once per instantiation, which
    the callers aggregate deliberately.
    """
    for block in entry.findall("lines"):
        yield from block.findall("line")


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
        for line in class_lines(entry):
            number = line.get("number")
            if number is None:
                continue
            count = int(line.get("hits", 0))
            # A template instantiated twice can leave two entries for one source
            # line. The line was executed if any instantiation ran it, so the max
            # is the answer; last-wins would let a never-taken instantiation mark
            # a covered line uncovered.
            lines[int(number)] = max(lines.get(int(number), 0), count)
        if lines:
            hits[name] = lines
    return hits


def read_branch_report(path):
    """{repo-relative path: {lineno: (covered, total)}} from Cobertura XML.

    `{}` means the report parsed and contains no branch data (a measurement gap
    the gate fails closed on); None means the file could not be read or a branch
    line's counts could not be parsed. A line without `condition-coverage` has no
    conditions and is absent, not zero.
    """
    try:
        root = ElementTree.parse(path).getroot()
    except (OSError, ElementTree.ParseError):
        return None
    branches = {}
    for entry in root.iter("class"):
        name = entry.get("filename")
        if not name:
            continue
        lines = {}
        for line in class_lines(entry):
            number = line.get("number")
            if number is None:
                continue
            condition = line.get("condition-coverage")
            if condition is None:
                if line.get("branch") == "true":
                    # A branch line without its aggregate is a format we do not
                    # understand; guessing zero would invent a finding.
                    return None
                continue
            match = CONDITION.match(condition.strip())
            if match is None:
                return None
            covered, total = int(match.group(2)), int(match.group(3))
            if covered > total:
                return None
            previous = lines.get(int(number), (0, 0))
            # Sum across instantiation entries: a condition exercised in one
            # instantiation and not another is half-covered, which is what gcovr's
            # own branch rate counts.
            lines[int(number)] = (previous[0] + covered, previous[1] + total)
        if lines:
            branches[name] = lines
    return branches


def patch_coverage(hits, changed, carried_over=None, absent_ok=(), branches=None, source=None):
    """Coverage over the added lines.

    `changed` is {path: {lineno: text}}; None means no diff was supplied, which is
    a measurement failure, not a clean patch. `carried_over` is {path: set of
    normalised texts already present at the base revision}. `source` is
    {path: {lineno: text}} for the decision-line filter; None reads the working
    tree.
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
    if branches is not None:
        result.branches = branch_coverage(branches, changed, carried_over, absent_ok, source)
    return result


def branch_coverage(branches, changed, carried_over=None, absent_ok=(), source=None):
    """Conditions on the added decision lines, gated scope pooled separately.

    A moved line is not new code, so none of its conditions count -- a line that
    moved with one side still uncovered would otherwise be charged to the patch
    that only relocated it. The rule is as generous as the line carryover for the
    same reason: a false positive blocks a refactor over code that was already
    there.

    Only decision lines count (see `is_decision_line`): a constructor's exception
    edges are not a code path the patch introduced.
    """
    carried_over = carried_over or {}
    if source is None:
        source = load_sources([path for path in changed if path in branches])
    report_empty = not any(branches.values())
    total = covered = carried = 0
    uncovered = []
    by_file = {}
    for path, numbers in changed.items():
        if not is_instrumented(path) or deliberately_not_measured(path, absent_ok):
            continue
        file_branches = branches.get(path)
        if not file_branches:
            continue
        known = carried_over.get(path, frozenset())
        file_covered = file_total = 0
        for number in sorted(numbers):
            pair = file_branches.get(number)
            if pair is None or pair[1] <= 0:
                continue
            if not is_decision_line(source.get(path), number):
                continue
            if normalise(numbers[number]) in known:
                carried += pair[1]
                continue
            file_covered += pair[0]
            file_total += pair[1]
            if pair[0] < pair[1]:
                uncovered.append((path, number, pair[0], pair[1]))
        if file_total:
            by_file[path] = (file_covered, file_total)
            if is_branch_gated(path):
                covered += file_covered
                total += file_total
    return BranchCoverage(total=total, covered=covered, uncovered=uncovered,
                          carried=carried, by_file=by_file, report_empty=report_empty)


def branch_ratchet(branches, baseline, source=None):
    """Files whose uncovered decision-branch count grew beyond the floor.

    The floor is the recorded count plus `BRANCH_RATCHET_SLACK`: coverage counts
    move between identical runs because some tests are timing-dependent, so a
    zero-growth rule would fail a clean tree. The patch bar is what catches a
    single new untested branch; this ratchet catches a file that keeps getting
    worse. A file the baseline does not name has a floor of zero (plus the
    slack), which is what makes a new source file prove most of its branches
    rather than inherit an allowance. Constructor/exception edges are not
    decisions and are excluded by the same filter the patch bar uses.
    """
    if source is None:
        source = load_sources([path for path in (branches or {}) if is_branch_gated(path)])
    violations = []
    for path, lines in sorted((branches or {}).items()):
        if not is_branch_gated(path):
            continue
        line_source = source.get(path)
        covered = total = 0
        for number, (covered_count, total_count) in lines.items():
            if not is_decision_line(line_source, number):
                continue
            covered += covered_count
            total += total_count
        uncovered = total - covered
        floor = baseline.get(path, 0)
        if uncovered > floor + BRANCH_RATCHET_SLACK:
            violations.append((path, uncovered, floor))
    return violations


def load_branch_baseline(path):
    """{path: uncovered count} from the committed baseline, or None when the file
    is missing or malformed. None is a measurement failure: an empty floor would
    silently tolerate every regression."""
    try:
        with open(path, encoding="utf-8") as handle:
            payload = json.load(handle)
    except (OSError, ValueError):
        return None
    files = payload.get("files") if isinstance(payload, dict) else None
    if not isinstance(files, dict):
        return None
    try:
        return {str(name): int(count) for name, count in files.items()}
    except (TypeError, ValueError):
        return None


def write_branch_baseline(path, branches, source=None):
    """Rewrite the baseline with every gated file that still has uncovered
    decision branches. A file that reached zero is omitted on purpose: its floor
    is zero by default, so it cannot regress unnoticed."""
    if source is None:
        source = load_sources([name for name in (branches or {}) if is_branch_gated(name)])
    counts = {}
    for name, lines in sorted((branches or {}).items()):
        if not is_branch_gated(name):
            continue
        line_source = source.get(name)
        covered = total = 0
        for number, (covered_count, total_count) in lines.items():
            if not is_decision_line(line_source, number):
                continue
            covered += covered_count
            total += total_count
        if total > covered:
            counts[name] = total - covered
    with open(path, "w", encoding="utf-8") as handle:
        json.dump({"_comment": BASELINE_COMMENT, "files": counts}, handle,
                  indent=2, sort_keys=True)
        handle.write("\n")


def check_branches(branches):
    """Print the branch verdict and return its exit code.

    A report with no branch data at all is the measurement gap; a patch with no
    new decisions is a legitimate pass. Those look identical in the totals, so
    the report flag is what separates them.
    """
    if branches.report_empty:
        print("coverage: FAILED CLOSED - the coverage report contains no branch data, "
              "so the branch gate would be measuring nothing. gcovr emits "
              "condition-coverage on branch lines; regenerate the report.")
        return 2
    if not branches.total:
        print("coverage: patch branch coverage - no new branches in lib/ tools/ plugins/ "
              "(the bar applies to decisions, not to straight-line code)")
    else:
        verdict = "ok" if branches.passes else "BELOW the bar"
        moved = f", {branches.carried} carried over on moved lines" if branches.carried else ""
        print(f"coverage: patch branch coverage {branches.percent:.1f}% "
              f"({branches.covered}/{branches.total}{moved}) - {verdict} "
              f"(bar: >= {BRANCH_COVERAGE_MIN}%, lib/ tools/ plugins/)")
        if branches.uncovered:
            shown = ", ".join(f"{p}:{n} ({c}/{t})" for p, n, c, t in branches.uncovered[:15])
            more = "" if len(branches.uncovered) <= 15 \
                else f" (+{len(branches.uncovered) - 15} more)"
            print(f"  uncovered added branches: {shown}{more}")
    for path, (covered, total) in sorted(branches.by_file.items()):
        if is_branch_gated(path):
            continue
        print(f"  {path}: {100.0 * covered / total:.1f}% ({covered}/{total}, report only)")
    return 0 if branches.passes else 1


def check(result, changed=None, ratchet=None, ratchet_error=None, ratchet_report_only=False):
    """Fail closed: a result with nothing to measure is an error, not a pass.
    The one exception is a diff that genuinely adds no lines (docs, whitespace),
    which is a legitimate pass and is signalled by changed being an empty dict.

    `ratchet_report_only` prints a violation and leaves the verdict alone: the
    baseline is measured with the pinned CI compiler, so a local run on a
    different GCC must not fail on compiler-generated counts.
    """
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
    rc = 0
    if result.total == 0:
        # Say so when the reason was carryover rather than an empty diff: "nothing
        # to gate" reads like the diff was empty when in fact lines moved.
        if result.carried:
            print(f"coverage: no new executable lines - {result.carried} added line(s) "
                  f"already existed elsewhere in the file (moved code is not new code)")
        else:
            print("coverage: no changed executable lines in the diff; nothing to gate "
                  f"(the {COVERAGE_MIN}% rule applies to code, not to docs)")
    else:
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
        if not result.passes:
            rc = 1
    if result.branches is not None:
        rc = max(rc, check_branches(result.branches))
    if ratchet_error is not None:
        print(f"coverage: FAILED CLOSED - {ratchet_error}")
        return 2
    if ratchet:
        print(f"coverage: {'NOTE' if ratchet_report_only else 'FAILED'} - {len(ratchet)} "
              f"file(s) carry more uncovered branches than the baseline records:")
        for path, uncovered, floor in ratchet[:15]:
            print(f"  {path}: {uncovered} uncovered (baseline {floor}, "
                  f"slack {BRANCH_RATCHET_SLACK})")
        if len(ratchet) > 15:
            print(f"  (+{len(ratchet) - 15} more)")
        if ratchet_report_only:
            print("  report only: the baseline is measured with CI's pinned compiler "
                  "(gcc 13), and branch counts are compiler-generated. CI enforces it.")
        else:
            print("  the baseline may only shrink; cover the new branches, or make the "
                  "reduction deliberate and regenerate it with "
                  "`make coverage-branch-baseline`")
            rc = max(rc, 1)
    return rc


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
                                  capture_output=True, text=True, **GIT_DECODE)
        except OSError:
            continue
        if proc.returncode == 0:
            out[path] = {normalise(l) for l in proc.stdout.split("\n")}
    return out


# git output is bytes, not text. A diff can legitimately contain invalid UTF-8 -- a fuzz
# corpus seed has to stay invalid, because that is what it is testing -- and decoding
# with the default codec raised UnicodeDecodeError on a PR that changed one. That is the
# gate crashing rather than reporting, which is better than a false pass but is not a
# measurement either. Decoding leniently keeps the line structure the parser needs.
GIT_DECODE = {"errors": "replace"}


def diff_against(base, path="."):
    """(diff lines, error). Empty output with an error means we must not gate."""
    try:
        proc = subprocess.run(["git", "-C", path, "diff", "--unified=0", base],
                              capture_output=True, text=True, **GIT_DECODE)
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
        # Same shape the diff path produces -- {path: {lineno: text}} -- because
        # patch_coverage indexes it as a mapping. It used to build {path: set(lines)},
        # so every use raised TypeError: a documented option that had never worked, and
        # only reachable by running it.
        return {p: {int(n): "" for n in v} for p, v in raw.items()}, None
    if args.diff_base:
        lines, err = diff_against(args.diff_base)
        if err:
            return None, err
        return changed_lines_from_diff(lines), None
    return None, "neither --changed-lines nor --diff-base was given"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--report", required=True, help="gcovr Cobertura XML report")
    ap.add_argument("--diff-base", help="git ref to diff against, e.g. origin/main")
    ap.add_argument("--changed-lines",
                    help="JSON {path: [line numbers]} instead of a diff")
    ap.add_argument("--markdown", help="write a one-line summary here (for the report)")
    ap.add_argument("--absent-ok", action="append", default=[], metavar="REGEX",
                    help="changed source matching this is excluded from the coverage "
                         "report on purpose; not a measurement gap (repeatable)")
    ap.add_argument("--branch-baseline", metavar="PATH",
                    help="uncovered-branch counts the gate ratchets against "
                         "(tests/branch_baseline.json)")
    ap.add_argument("--branch-ratchet-report-only", action="store_true",
                    help="print ratchet violations without failing; for a local run "
                         "on a compiler the baseline was not measured with")
    ap.add_argument("--update-branch-baseline", metavar="PATH",
                    help="rewrite the branch baseline from --report and exit; only a "
                         "deliberate reduction should run this")
    args = ap.parse_args()

    branches = read_branch_report(args.report)
    if args.update_branch_baseline:
        if branches is None:
            print(f"coverage: FAILED CLOSED - cannot read branch data from "
                  f"{args.report}; refusing to record a baseline from a report the "
                  f"gate cannot parse.", file=sys.stderr)
            return 2
        if not any(branches.values()):
            print("coverage: FAILED CLOSED - the report contains no branch data; a "
                  "baseline of zeros would tolerate every regression.", file=sys.stderr)
            return 2
        write_branch_baseline(args.update_branch_baseline, branches)
        print(f"coverage: wrote {args.update_branch_baseline} from {args.report}")
        return 0

    changed, err = resolve_changed(args)
    if err:
        print(f"coverage: FAILED CLOSED - {err}", file=sys.stderr)
        return 2
    carried_over = base_lines(args.diff_base, changed) if args.diff_base else {}
    result = patch_coverage(read_report(args.report), changed, carried_over,
                            tuple(args.absent_ok), branches)
    ratchet, ratchet_error = None, None
    if args.branch_baseline:
        baseline = load_branch_baseline(args.branch_baseline)
        if branches is None:
            ratchet_error = ("the coverage report has no readable branch data, so the "
                             "branch ratchet cannot be evaluated")
        elif baseline is None:
            ratchet_error = f"could not read the branch baseline {args.branch_baseline}"
        else:
            ratchet = branch_ratchet(branches, baseline)
    rc = check(result, changed, ratchet, ratchet_error, args.branch_ratchet_report_only)
    if args.markdown and result.percent is not None:
        write_markdown(args.markdown, result)
    return rc


def write_markdown(path, result):
    with open(path, "a", encoding="utf-8") as handle:
        handle.write(f"| coverage (patch) | {'pass' if result.passes else 'fail'} | "
                     f"{result.percent:.1f}% of {result.total} added line(s) "
                     f"(bar {COVERAGE_MIN}%) |\n")
        branches = result.branches
        if branches is not None and branches.total:
            handle.write(f"| coverage (branch) | "
                         f"{'pass' if branches.passes else 'fail'} | "
                         f"{branches.percent:.1f}% of {branches.total} added branch(es) "
                         f"(bar {BRANCH_COVERAGE_MIN}%) |\n")


if __name__ == "__main__":
    sys.exit(main())