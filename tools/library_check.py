#!/usr/bin/env python3
"""Run the library's `run` blocks.

An entry may carry a `## Run it` section holding fenced ```run blocks -- the
commands that make its claim checkable. Each block is executed with bash in a
fresh scratch directory, and a non-zero exit fails the check: an entry whose
commands no longer work is knowledge that has silently gone stale.

A block whose first line begins `# manual:` is skipped on purpose. It needs a
project, a cluster or a machine this check does not have, and saying so is honest
where a silent skip is not.

Exit codes: 0 clean, 1 a block failed, 2 the check could not run (no library).
Fail closed: a measurement that covers nothing is not a pass.

Usage: library_check.py [--dir <library dir>] [--verbose]
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

FENCE = re.compile(r"^```run\s*$")
CLOSE = re.compile(r"^```\s*$")
MANUAL = re.compile(r"^\s*#\s*manual:")


def library_dir(root):
    candidate = os.path.join(root, "prompts", "domains", "coding", "library")
    return candidate if os.path.isdir(candidate) else None


def entry_files(directory):
    """Every entry under the library, topics included; the vocabulary is not one."""
    found = []
    for base, dirs, files in os.walk(directory):
        dirs[:] = sorted(d for d in dirs if not d.startswith("."))
        for name in sorted(files):
            if name.endswith(".md") and name != "tags.md":
                found.append(os.path.join(base, name))
    return sorted(found)


def run_blocks(path):
    """(line number, text) for each ```run block in an entry."""
    with open(path, encoding="utf-8") as handle:
        lines = handle.read().split("\n")
    blocks = []
    index = 0
    while index < len(lines):
        if FENCE.match(lines[index]):
            start = index + 1
            end = start
            while end < len(lines) and not CLOSE.match(lines[end]):
                end += 1
            blocks.append((start + 1, "\n".join(lines[start:end])))
            index = end + 1
            continue
        index += 1
    return blocks


def is_manual(block):
    for line in block.split("\n"):
        if not line.strip():
            continue
        return bool(MANUAL.match(line))
    return False


def execute(block, workdir):
    """Run one block; returns (ok, combined output)."""
    result = subprocess.run(["bash", "-e", "-o", "pipefail", "-c", block], cwd=workdir,
                            capture_output=True, text=True, timeout=300)
    return result.returncode == 0, result.stdout + result.stderr


def main(argv):
    parser = argparse.ArgumentParser(description="Run the library's run blocks")
    parser.add_argument("--dir", dest="directory", default=None)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    directory = args.directory or library_dir(root)
    if not directory or not os.path.isdir(directory):
        print("library-check: no library directory found", file=sys.stderr)
        return 2

    entries = entry_files(directory)
    if not entries:
        print("library-check: no entries found", file=sys.stderr)
        return 2

    ran = 0
    manual = 0
    failures = []
    for path in entries:
        for line, block in run_blocks(path):
            if is_manual(block):
                manual += 1
                if args.verbose:
                    print(f"  manual   {os.path.relpath(path, root)}:{line}")
                continue
            with tempfile.TemporaryDirectory(prefix="library_check_") as work:
                try:
                    ok, output = execute(block, work)
                except subprocess.TimeoutExpired:
                    ok, output = False, "timed out"
            ran += 1
            if ok:
                if args.verbose:
                    print(f"  pass     {os.path.relpath(path, root)}:{line}")
            else:
                failures.append((path, line, output))

    for path, line, output in failures:
        print(f"library-check: FAILED {os.path.relpath(path, root)}:{line}", file=sys.stderr)
        for text in output.strip().split("\n")[-12:]:
            print("    " + text, file=sys.stderr)

    print(f"library-check: {len(entries)} entries, {ran} block(s) run, {manual} manual, "
          f"{len(failures)} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
