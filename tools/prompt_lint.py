#!/usr/bin/env python3
"""The prompting philosophy, checked.

`prompts/` is descriptive: it describes the role, the environment and the tools,
and leaves the agent free to work. Prohibitive phrasing ("never", "don't", "do
not", "must") is the thing the rule forbids, and it is also a leading indicator:
a prompt that has to forbid a behaviour is usually a prompt missing the context
that would make the behaviour unnecessary.

Scope is the agent-facing prompt set: `prompts/*.md`, `prompts/tools/`,
`prompts/watch/` and other domains. `prompts/domains/coding/library/` is
reference knowledge an agent consults, not an instruction, so it is not held to
the same voice.

Sentence-initial only. "what you do not know, you mark as such" is descriptive;
"Do not guess." is a prohibition. The gate flags the second and leaves the first,
which is the distinction the philosophy is about.

Usage:
  tools/prompt_lint.py --check
  tools/prompt_lint.py --report
"""

import argparse
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SCOPED_ROOT = "prompts"
EXCLUDED_PREFIXES = ("prompts/domains/coding/library/",)

# A sentence boundary (start, after .!?, or a Markdown list/heading marker) and
# then a prohibitive modal. `(?<=[.!?])\s+` keeps the lookbehind one character,
# which Python supports.
BANNED = re.compile(
    r"(?:^|(?<=[.!?])\s+|^[-*#]+\s+)(never|don'?t|do not|must)\b",
    re.IGNORECASE | re.MULTILINE,
)

# A scan that read almost nothing must not report a clean prompt set.
MIN_FILES_SANE = 10


def is_scoped(rel):
    if not rel.startswith(SCOPED_ROOT + "/") or not rel.endswith(".md"):
        return False
    return not rel.startswith(EXCLUDED_PREFIXES)


def scoped_files(root=REPO_ROOT):
    found = []
    for base, dirs, files in os.walk(os.path.join(root, SCOPED_ROOT)):
        dirs[:] = sorted(d for d in dirs if not d.startswith("."))
        for name in sorted(files):
            rel = os.path.relpath(os.path.join(base, name), root)
            if is_scoped(rel):
                found.append(rel)
    return sorted(found)


def findings(text):
    """(line number, matched phrase) for each prohibitive sentence."""
    out = []
    in_fence = False
    for number, line in enumerate(text.split("\n"), start=1):
        if line.strip().startswith("```"):
            in_fence = not in_fence
            continue
        if in_fence:
            continue
        for match in BANNED.finditer(line):
            out.append((number, match.group(0).strip()))
    return out


def scan(root=REPO_ROOT):
    """({relative path: findings}, files scanned)."""
    found = {}
    scanned = 0
    for rel in scoped_files(root):
        scanned += 1
        with open(os.path.join(root, rel), encoding="utf-8", errors="replace") as handle:
            hits = findings(handle.read())
        if hits:
            found[rel] = hits
    return found, scanned


def check(found, scanned):
    if scanned < MIN_FILES_SANE:
        print(f"prompt-lint: FAILED CLOSED - scanned only {scanned} prompt(s), expected "
              f"at least {MIN_FILES_SANE}.", file=sys.stderr)
        return 2
    if not found:
        print(f"prompt-lint: {scanned} prompt(s) scanned, 0 prohibitive sentences - clean")
        return 0
    total = sum(len(v) for v in found.values())
    print(f"prompt-lint: {total} prohibitive sentence(s) in {len(found)} prompt(s):")
    for rel, hits in sorted(found.items()):
        for number, phrase in hits:
            print(f"  {rel}:{number}: {phrase}")
    print("  Describe the role and the environment instead; a prohibition is usually a "
          "missing reason. `prompts/domains/coding/library/` is reference knowledge and "
          "is not scoped.")
    return 1


def report(found, scanned):
    print(f"prompts scanned: {scanned}")
    print(f"prohibitive sentences: {sum(len(v) for v in found.values())}")
    for rel, hits in sorted(found.items()):
        for number, phrase in hits:
            print(f"  {rel}:{number}: {phrase}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument("--check", action="store_true", help="gate: no prohibitive prompt sentences")
    group.add_argument("--report", action="store_true", help="list findings")
    args = ap.parse_args()

    found, scanned = scan()
    if args.report:
        report(found, scanned)
        return 0
    return check(found, scanned)


if __name__ == "__main__":
    sys.exit(main())
