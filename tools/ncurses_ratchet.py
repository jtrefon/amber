#!/usr/bin/env python3
"""Ratchet: only allowlisted files may include ncurses.

The shell's target is one terminal adapter: every other TUI module paints into a
cell grid and never sees a curses type. Moving 18 existing files to that shape is
a migration, so the set is frozen in `tests/ncurses_baseline.json` and can only
shrink. A new include in a file that is not on the list fails; removing one from a
listed file is recorded with `--update` (the file then leaves the list), so the
ratchet never silently re-grows.

A gate that measures nothing reports green. The scan therefore has a floor
(`--min-files`) and a missing baseline is an error, never a pass.

Usage:
  tools/ncurses_ratchet.py              # check (CI / make ncurses)
  tools/ncurses_ratchet.py --report     # counts plus the current list
  tools/ncurses_ratchet.py --update     # record the (smaller) set
"""

import argparse
import json
import os
import re
import subprocess
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE = os.path.join("tests", "ncurses_baseline.json")
CODE_SUFFIXES = (".h", ".hpp", ".c", ".cc", ".cpp")
MIN_FILES_SANE = 200

INCLUDE = re.compile(
    r'^\s*#\s*include\s*[<"](ncurses|ncursesw|panel|form|menu)\.h[>"]', re.MULTILINE
)


def tracked_files(root):
    """Code files under `root`. A git tree is read through `git ls-files` so
    build output and untracked scratch files cannot satisfy or trip the scan; a
    plain directory (the gate's own tests) is walked instead."""
    if os.path.isdir(os.path.join(root, ".git")) or os.path.isfile(
        os.path.join(root, ".git")
    ):
        out = subprocess.run(
            ["git", "-C", root, "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        ).stdout
        names = [name for name in out.decode("utf-8", "replace").split("\0") if name]
    else:
        names = []
        for base, _dirs, files in os.walk(root):
            for name in files:
                path = os.path.join(base, name)
                names.append(os.path.relpath(path, root).replace(os.sep, "/"))
    return sorted(name for name in names if name.endswith(CODE_SUFFIXES))


def scan(root):
    """relative path -> line numbers of a curses include."""
    found = {}
    files = tracked_files(root)
    for name in files:
        path = os.path.join(root, name)
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as handle:
                text = handle.read()
        except OSError as exc:
            raise RuntimeError(f"cannot read {name}: {exc}") from exc
        lines = [text.count("\n", 0, match.start()) + 1 for match in INCLUDE.finditer(text)]
        if lines:
            found[name] = lines
    return found, len(files)


def load_baseline(path):
    try:
        with open(path, "r", encoding="utf-8") as handle:
            payload = json.load(handle)
    except FileNotFoundError as exc:
        raise RuntimeError(f"baseline not found: {path}") from exc
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"baseline is unreadable: {path}: {exc}") from exc
    files = payload.get("files") if isinstance(payload, dict) else None
    if not isinstance(files, list) or not all(isinstance(name, str) for name in files):
        raise RuntimeError(f"baseline has no files list: {path}")
    return set(files)


def check(found, baseline, scanned, min_files=MIN_FILES_SANE):
    if scanned < min_files:
        print(
            f"ncurses-ratchet: FAILED CLOSED - scanned only {scanned} file(s), "
            f"expected at least {min_files}."
        )
        return 2
    current = set(found)
    added = sorted(current - baseline)
    removed = sorted(baseline - current)
    for name in added:
        locations = ", ".join(str(line) for line in found[name])
        print(f"ncurses-ratchet: new ncurses include in {name} (line {locations})")
    if added:
        print(
            "The shell has one terminal adapter: paint into tui::Surface instead of "
            "calling ncurses, or move the adapter itself."
        )
        return 1
    for name in removed:
        print(f"ncurses-ratchet: {name} no longer includes ncurses (run --update to shrink)")
    print(
        f"ncurses-ratchet: clean ({len(found)} file(s) allowed, "
        f"{scanned} scanned, {len(removed)} ready to drop)"
    )
    return 0


def update(path, found):
    payload = {
        "_comment": "Files allowed to include ncurses; regenerate with tools/ncurses_ratchet.py --update.",
        "files": sorted(found),
    }
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(payload, handle, indent=2)
        handle.write("\n")
    print(f"ncurses-ratchet: recorded {len(found)} file(s) in {path}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=REPO_ROOT)
    parser.add_argument("--baseline", default=None)
    parser.add_argument("--min-files", type=int, default=MIN_FILES_SANE)
    parser.add_argument("--check", action="store_true", help="check (the default)")
    parser.add_argument("--report", action="store_true")
    parser.add_argument("--update", action="store_true")
    args = parser.parse_args(argv)
    baseline_path = args.baseline or os.path.join(args.root, BASELINE)
    try:
        found, scanned = scan(args.root)
        if args.update:
            update(baseline_path, found)
            return 0
        if args.report:
            print(f"ncurses-ratchet: {len(found)} file(s) include ncurses")
            for name in sorted(found):
                print(f"  {name}")
            return 0
        baseline = load_baseline(baseline_path)
    except (RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"ncurses-ratchet: {exc}")
        return 2
    return check(found, baseline, scanned, args.min_files)


if __name__ == "__main__":
    sys.exit(main())
