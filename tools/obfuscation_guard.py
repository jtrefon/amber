#!/usr/bin/env python3
"""Obfuscated-payload guard.

Every other gate in this repo reads a *build* or a *metric*. None of them read
file contents, which is why an obfuscated JavaScript dropper could sit in
`website/` for months: `astro build` was green, the website smoke check only
asserted HTTP status codes, and nothing looked at the source.

This scans the tracked tree for the two things that payload could not hide:

  1. A known obfuscator marker. The blob appended to website/astro.config.mjs,
     website/scripts/add-base.mjs and website/scripts/smoke.mjs (introduced by
     f382c38, removed in the commit that added this file) used a rotating
     string array, `String.fromCharCode(127)` separators, `_$_` identifiers and
     `global.<x> = require` to bootstrap CommonJS from an ES module.
  2. A line long enough that it cannot be hand-written source. The injected
     blob was ~6.4 KB on a single line; no reviewed line in this repo is that
     long. Minified/vendored code is excluded by path, not by length.

Tracked files only (`git ls-files`): build output and node_modules are not
ours to police, and scanning them would make the gate slow and noisy.

Usage:
  tools/obfuscation_guard.py            # scan (CI / make check)
  tools/obfuscation_guard.py --list     # show what it would scan
"""

import argparse
import os
import subprocess
import sys

# High-signal strings observed in the injected blob. Each one alone is enough:
# none of these appear in hand-written source in this project.
MARKERS = (
    "String.fromCharCode(127)",
    "_$_",
    "global.i=",
    "global.o=",
    "global.r=require",
    "global.m=module",
)

# No reviewed line of CODE is this long. The injected blob was ~6400 chars on a
# single line.
#
# The length rule applies to code only. This repo also tracks data — bench
# result dumps, ASCII art, completions.json — whose lines are legitimately
# long, so measuring those would produce nothing but noise.
MAX_LINE = 500

CODE_EXTENSIONS = (
    ".c", ".cc", ".cpp", ".h", ".hpp",
    ".js", ".mjs", ".cjs", ".jsx", ".ts", ".tsx",
    ".py", ".sh", ".bash", ".zsh", ".rb", ".astro",
)
DATA_EXTENSIONS = (".html", ".css", ".json", ".yml", ".yaml")
SCAN_EXTENSIONS = CODE_EXTENSIONS + DATA_EXTENSIONS

# Vendored code and generated lockfiles are not ours to police.
SKIP_PARTS = ("third_party", "node_modules", "vendor", "nlohmann")
SKIP_NAMES = ("package-lock.json",)


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def tracked_files(root):
    proc = subprocess.run(
        ["git", "ls-files", "-z"], cwd=root, capture_output=True, text=True
    )
    if proc.returncode != 0:
        print("obfuscation-guard: FAILED CLOSED - not a git work tree", file=sys.stderr)
        sys.exit(2)
    return [p for p in proc.stdout.split("\0") if p]


def scannable(path):
    if os.path.basename(path) in SKIP_NAMES:
        return False
    if not path.endswith(SCAN_EXTENSIONS):
        return False
    return not any(part in path.split("/") for part in SKIP_PARTS)


def scan_file(root, path):
    """Findings for one file, as (line number, description)."""
    findings = []
    check_length = path.endswith(CODE_EXTENSIONS)
    try:
        with open(os.path.join(root, path), encoding="utf-8", errors="ignore") as handle:
            for number, line in enumerate(handle, start=1):
                stripped = line.rstrip("\n")
                for marker in MARKERS:
                    if marker in stripped:
                        findings.append((number, f"obfuscator marker {marker!r}"))
                        break
                else:
                    if check_length and len(stripped) > MAX_LINE:
                        findings.append(
                            (number, f"{len(stripped)}-char line (limit {MAX_LINE})")
                        )
    except OSError as exc:
        findings.append((0, f"unreadable: {exc}"))
    return findings


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--list", action="store_true", help="show the files scanned")
    args = parser.parse_args()

    root = repo_root()
    paths = [p for p in tracked_files(root) if scannable(p)]

    if args.list:
        for path in paths:
            print(path)
        print(f"obfuscation-guard: {len(paths)} file(s) would be scanned")
        return 0

    hits = []
    for path in paths:
        for number, description in scan_file(root, path):
            hits.append(f"  {path}:{number}: {description}")

    if hits:
        print("obfuscation-guard: obfuscated payload detected")
        print("\n".join(hits))
        print(
            "  This is not a style issue: an obfuscated blob in tracked source is\n"
            "  how the website payload was injected (commit f382c38). Do not commit\n"
            "  it, do not run it, and report it."
        )
        return 1

    print(f"obfuscation-guard: clean ({len(paths)} tracked file(s) scanned)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
