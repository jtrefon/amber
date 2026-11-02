#!/usr/bin/env python3
"""One version, one owner.

`version.txt` is the only place the release version is written, and a release
tag must agree with it. Both release workflows build from a tag, and they used
to disagree about who wins: `release.yml` overwrote `version.txt` from the tag
while `packages.yml` refused to build when the two differed. A tag cut without
the stamp therefore published a GitHub release and no distro package -- v0.8.3
did exactly that, and `amber update` kept installing 0.8.2.

Run this before creating a tag; the workflows run the same comparison:

  * shape  `X.Y.Z`, optionally with a `-suffix`. A branch name is not a tag.
  * owner  the tag, minus a leading `v`, must equal `version.txt`.

Usage:
  tools/release_check.py --tag v0.8.4
  tools/release_check.py --tag v0.8.4 --root /path/to/tree
"""

import argparse
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

TAG = re.compile(r"^v?[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z.-]+)?$")


def normalize_tag(tag):
    """The version a release tag names, or ValueError when it names none."""
    if not TAG.match(tag or ""):
        raise ValueError(f"'{tag}' is not a release tag")
    return tag.lstrip("v")


def read_version(root):
    """The stamped version: the first line of version.txt, whitespace removed."""
    with open(os.path.join(root, "version.txt"), encoding="utf-8") as handle:
        return handle.readline().strip()


def compare(stamped, version):
    """None when the tag and the tree agree, otherwise what a build must say."""
    if stamped == version:
        return None
    return (f"version.txt says {stamped}, the tag says {version}: "
            "stamp version.txt and commit it before cutting the tag")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--tag", default=os.environ.get("TAG", ""),
                    help="release tag, e.g. v0.8.4 (defaults to $TAG)")
    ap.add_argument("--root", default=REPO_ROOT,
                    help="tree whose version.txt is checked")
    args = ap.parse_args()

    try:
        version = normalize_tag(args.tag)
    except ValueError as bad:
        print(f"release-check: {bad}", file=sys.stderr)
        return 2
    try:
        stamped = read_version(args.root)
    except OSError as missing:
        print(f"release-check: cannot read version.txt: {missing}", file=sys.stderr)
        return 2

    problem = compare(stamped, version)
    if problem:
        print(f"release-check: {problem}", file=sys.stderr)
        return 1
    print(f"release-check: {version} matches version.txt")
    return 0


if __name__ == "__main__":
    sys.exit(main())
