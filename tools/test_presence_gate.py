#!/usr/bin/env python3
"""A production change must come with a test change, or a waiver.

This is the enforceable half of "bug fixes start with a failing test". The gate
cannot know the test was written first, but it can know whether the change
touched the suite at all -- which is the failure that actually happens: a fix
lands with no test, and the next refactor breaks it silently.

Production paths are the compiled sources (`lib/`, `tui/`, `src/`, `tools/`,
`plugins/`, `bench/`). `bench/scenarios/` is excluded: that is code the benchmark
*agent* writes and an oracle scores. Docs, prompts, workflows and the build
files are not production code.

A waiver is `tests-allow: <reason>` in the PR body or in a commit message, for a
change whose behaviour is already covered or genuinely cannot be tested yet.

Usage:
  tools/test_presence_gate.py --base origin/main
  PR_BODY='...' tools/test_presence_gate.py --base origin/main
"""

import argparse
import os
import subprocess
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

PRODUCTION_PREFIXES = ("lib/", "tui/", "src/", "tools/", "plugins/", "bench/")
PRODUCTION_SUFFIXES = (".c", ".cc", ".cpp", ".h", ".hpp", ".hxx")
EXCLUDED_PREFIXES = ("bench/scenarios/",)
TEST_PREFIX = "tests/"
WAIVER = "tests-allow:"

GIT = {"errors": "replace"}


def production_paths(files):
    """Compiled project sources whose behaviour the suite is supposed to cover."""
    found = []
    for path in files:
        if not path.endswith(PRODUCTION_SUFFIXES):
            continue
        if path.startswith(EXCLUDED_PREFIXES):
            continue
        if path.startswith(PRODUCTION_PREFIXES):
            found.append(path)
    return found


def assess(files, body="", commits=""):
    """Production files to complain about; empty means the rule is satisfied."""
    production = production_paths(files)
    if not production:
        return []
    if any(path.startswith(TEST_PREFIX) for path in files):
        return []
    if WAIVER in body or WAIVER in commits:
        return []
    return production


def _git(*args):
    proc = subprocess.run(["git", "-C", REPO_ROOT, *args],
                          capture_output=True, text=True, **GIT)
    return proc


def changed_files(base):
    """The union the local gates use: committed, staged, unstaged and untracked."""
    commands = (
        ("diff", "--name-only", f"{base}...HEAD"),
        ("diff", "--name-only", "--cached", "HEAD"),
        ("diff", "--name-only", "HEAD"),
        ("ls-files", "--others", "--exclude-standard"),
    )
    found = set()
    for command in commands:
        proc = _git(*command)
        if proc.returncode != 0 and command[0] == "diff" and "..." in command[-1]:
            return None, proc.stderr.strip()[:200] or "git diff failed"
        found.update(line for line in proc.stdout.splitlines() if line)
    return sorted(found), None


def commit_text(base):
    proc = _git("log", "--format=%B", f"{base}..HEAD")
    return proc.stdout if proc.returncode == 0 else ""


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--base", default=os.environ.get("BASE", ""),
                    help="git ref to diff against (defaults to $BASE)")
    ap.add_argument("--body", default=os.environ.get("PR_BODY", ""),
                    help="PR body; a `tests-allow:` line waives the gate")
    args = ap.parse_args()

    if not args.base:
        print("test-presence: FAILED CLOSED - no base ref", file=sys.stderr)
        return 2
    files, error = changed_files(args.base)
    if error:
        print(f"test-presence: FAILED CLOSED - {error}", file=sys.stderr)
        return 2
    if files is None:
        return 2

    production = assess(files, args.body, commit_text(args.base))
    if not production:
        print("test-presence: the change touches tests, or is not production code")
        return 0

    print("test-presence: production code changed without a test change:")
    for path in production:
        print(f"  {path}")
    print("  Add a test under tests/ (preferably the red one that failed first), or "
          f"put `{WAIVER} <reason>` in the PR body or a commit message.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
