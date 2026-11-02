#!/usr/bin/env python3
"""Advisory commit hygiene: scoped subjects, and a red-test signal.

AGENTS.md asks for imperative subjects with a scoped prefix
(`tui: fix drawer scroll`) and a red test commit before the green fix. The
history does not meet that consistently yet, so this is advisory: the CI step
carries `continue-on-error: true`, the job concludes success, and the findings
are `::warning::` annotations. It never gates a merge.

  * subject     `<area>: <summary>`, <= 72 characters. Merge/revert/fixup are
                exempt; `fix(coding):` is reported because it is a different
                convention, not because it is wrong.
  * red signal  when production code changed, warn if no commit subject carries
                `(red)`. The gate cannot know the test failed, only that a
                red-marked test commit exists.

Usage:
  tools/commit_lint.py --base origin/main
  tools/commit_lint.py --base origin/main --tdd
"""

import argparse
import os
import re
import subprocess
import sys

import test_presence_gate

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAX_SUBJECT = 72

SUBJECT = re.compile(r"^[a-z][a-z0-9/_-]*: \S")
EXEMPT = ("Merge ", "Revert ", "fixup!", "squash!")
RED = re.compile(r"\(red\)", re.IGNORECASE)

# Findings are warning annotations on GitHub. The job concludes success (it is
# advisory), so a plain log line would be buried; an annotation puts the finding
# where a reviewer looks without making the check red.
ANNOTATION = "::warning::"


def finding_line(text, in_actions=None):
    """The line to print for a finding: annotated under GitHub Actions."""
    if in_actions is None:
        in_actions = os.environ.get("GITHUB_ACTIONS") == "true"
    return f"{ANNOTATION}{text}" if in_actions else text


def lint_subjects(subjects):
    """(index, subject, reason) for each subject that does not match the rule."""
    problems = []
    for index, subject in enumerate(subjects):
        subject = subject.strip()
        if not subject or subject.startswith(EXEMPT):
            continue
        if len(subject) > MAX_SUBJECT:
            problems.append((index, subject, f"longer than {MAX_SUBJECT} characters"))
        elif not SUBJECT.match(subject):
            problems.append((index, subject, "expected '<area>: <imperative summary>'"))
    return problems


def has_red(subjects):
    return any(RED.search(subject) for subject in subjects)


def tdd_warning(subjects, files):
    production = test_presence_gate.production_paths(files)
    if production and not has_red(subjects):
        return True
    return False


def _git(*args):
    proc = subprocess.run(["git", "-C", REPO_ROOT, *args],
                          capture_output=True, text=True, errors="replace")
    return proc


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--base", default=os.environ.get("BASE", ""),
                    help="git ref to inspect (defaults to $BASE)")
    ap.add_argument("--tdd", action="store_true",
                    help="warn when production changed without a `(red)` commit")
    args = ap.parse_args()

    if not args.base:
        print("commit-lint: FAILED CLOSED - no base ref", file=sys.stderr)
        return 2
    log = _git("log", "--reverse", "--format=%s", f"{args.base}..HEAD")
    if log.returncode != 0:
        print(f"commit-lint: FAILED CLOSED - {log.stderr.strip()[:200]}", file=sys.stderr)
        return 2
    subjects = [line.strip() for line in log.stdout.splitlines() if line.strip()]

    problems = lint_subjects(subjects)
    for index, subject, reason in problems:
        print(finding_line(f"commit-lint: {subject!r}: {reason}"))
    if args.tdd:
        diff = _git("diff", "--name-only", f"{args.base}...HEAD")
        files = [line for line in diff.stdout.splitlines() if line] \
            if diff.returncode == 0 else []
        if tdd_warning(subjects, files):
            print(finding_line(
                "commit-lint: production changed but no commit subject carries '(red)' "
                "(advisory: the red test commit is the evidence the fix was test-first)"))

    if problems:
        print(f"commit-lint: {len(problems)} subject(s) do not match the convention "
              "(advisory; not a merge gate)")
        return 1
    print(f"commit-lint: {len(subjects)} commit(s), subjects match")
    return 0


if __name__ == "__main__":
    sys.exit(main())
