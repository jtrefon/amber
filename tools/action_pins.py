#!/usr/bin/env python3
"""Every external GitHub Action is pinned to a commit SHA.

A tag like `actions/checkout@v7` is mutable: the tag can be moved to new code
without any change in this repository. A release pipeline that runs an action by
tag trusts whatever that tag points at today. The pin is the commit SHA; the tag
stays in a trailing comment so a human (and Dependabot) can still read which
release it is.

Local composite actions (`./.github/actions/...`) are part of this tree, so they
are not pinned. `docker://` references are image digests, a different mechanism.

Usage:
  tools/action_pins.py
"""

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
USE = re.compile(r"^\s*(?:-\s*)?uses:\s*(\S+)\s*(#.*)?$")
SHA = re.compile(r"^[^@]+@[0-9a-f]{40}$")
LOCAL_PREFIXES = ("./", "docker://")


def scan(root=REPO_ROOT):
    """[(path, line, use, reason)] for external actions that are not SHA-pinned.

    Reads workflows and composite-action manifests, because the install-deps
    action's cache steps are actions too.
    """
    violations = []
    for path in sorted(Path(root, ".github").rglob("*.yml")):
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            match = USE.match(line)
            if not match:
                continue
            use, comment = match.group(1), (match.group(2) or "").strip()
            if use.startswith(LOCAL_PREFIXES):
                continue
            relative = path.relative_to(root)
            if not SHA.match(use):
                violations.append((str(relative), number, use,
                                   "not a 40-character commit SHA"))
            elif not comment:
                violations.append((str(relative), number, use, "no tag comment"))
    return violations


def main():
    violations = scan()
    if violations:
        print(f"action-pins: FAILED - {len(violations)} external action(s) are not "
              f"pinned by SHA:")
        for path, line, use, reason in violations:
            print(f"  {path}:{line} {use} ({reason})")
        print("  pin to a full commit SHA and keep the tag in a trailing comment: "
              "`uses: owner/action@<sha> # vN`")
        return 1
    print("action-pins: every external action is pinned by SHA with a tag comment")
    return 0


if __name__ == "__main__":
    sys.exit(main())
