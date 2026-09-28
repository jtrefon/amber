#!/usr/bin/env python3
"""Select the translation units `make lint-changed` should analyse.

clang-tidy reports per translation unit, and a TU inherits every header it
includes, so a header change can introduce a finding in a TU the diff never
touched. Linting the whole tree for any header change is correct but makes the
gate all-or-nothing (a ~25 min run instead of ~1), so it can never be tightened.

This asks the compiler for each TU's real dependency list (`-MM`: it runs the
preprocessor but does not compile, ~0.4 s per TU) and selects the changed .cpp
files plus every TU whose includes reach a changed header. A TU whose
dependencies cannot be computed is selected, so a file that cannot be read is
analysed rather than skipped.

Usage:
  affected_tus.py --base origin/main --cxx clang++ --cxxflags "-Iinclude ..." \\
      $(LINT_SRCS)

Prints the selected .cpp paths, one per line, sorted.
"""

import argparse
import os
import shlex
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

# A .clang-tidy change alters how every TU is checked, so nothing can be skipped.
CONFIG_FILE = ".clang-tidy"
HEADER_SUFFIXES = (".h", ".hpp")


def is_header(path):
    return path.endswith(HEADER_SUFFIXES)


def normalise(path):
    """Repo-relative, forward-slashed, no leading './'."""
    path = path.replace(os.sep, "/")
    while path.startswith("./"):
        path = path[2:]
    return path


def depends_on(deps, changed_headers):
    """True when any changed header appears in a TU's dependency list.

    `-MM` prints the path it resolved (possibly './x' or absolute), so match on
    the tail rather than requiring an exact string. Over-matching only means a
    TU is analysed that did not need to be.
    """
    for dep in deps:
        for header in changed_headers:
            if dep == header or dep.endswith("/" + header):
                return True
    return False


def select(sources, changed_cpp, changed_headers, config_changed, deps_of):
    """The TUs to lint.

    `deps_of(src)` returns that TU's dependency paths, or None when they cannot
    be determined (the TU is then selected).
    """
    if config_changed:
        return list(sources)

    selected = {src for src in sources if src in changed_cpp}
    for src in sources:
        if src in selected or not changed_headers:
            continue
        deps = deps_of(src)
        if deps is None or depends_on(deps, changed_headers):
            selected.add(src)
    return sorted(selected)


def parse_deps(output):
    """The dependency paths from `cxx -MM` output, minus the target itself."""
    text = output.replace("\\\n", " ")
    _, _, rest = text.partition(":")
    return {normalise(tok) for tok in rest.split() if tok and not tok.endswith(":")}


def make_deps_reader(cxx, cxxflags):
    def deps_of(src):
        cmd = [cxx, "-MM", "-MG", *cxxflags, src]
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        except (OSError, subprocess.SubprocessError):
            return None
        if proc.returncode != 0 or not proc.stdout:
            return None
        return parse_deps(proc.stdout)

    return deps_of


def git_changed(base):
    """Repo-relative changed paths vs `base`, or None when git cannot tell."""
    try:
        proc = subprocess.run(
            ["git", "diff", "--name-only", f"{base}...HEAD"],
            capture_output=True,
            text=True,
            timeout=60,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if proc.returncode != 0:
        return None
    return [normalise(p) for p in proc.stdout.splitlines() if p.strip()]


def repo_root():
    """The git top level, or the cwd outside a repository."""
    try:
        proc = subprocess.run(["git", "rev-parse", "--show-toplevel"],
                              capture_output=True, text=True, timeout=60)
        if proc.returncode == 0 and proc.stdout.strip():
            return os.path.abspath(proc.stdout.strip())
    except (OSError, subprocess.SubprocessError):
        pass
    return os.getcwd()


def selftest():
    """Exercise the selection rules without git or a compiler."""
    sources = ["lib/a.cpp", "lib/b.cpp", "lib/c.cpp"]

    # A changed .cpp is always selected; a header pulls in its includers.
    deps = {
        "lib/a.cpp": ["include/agent/x.h"],
        "lib/b.cpp": ["include/agent/y.h"],
        "lib/c.cpp": ["include/agent/x.h", "include/agent/y.h"],
    }
    got = select(sources, {"lib/b.cpp"}, {"include/agent/x.h"}, False, deps.get)
    assert got == ["lib/a.cpp", "lib/b.cpp", "lib/c.cpp"], got

    # An unrelated header change selects nothing on its own.
    got = select(sources, set(), {"include/agent/z.h"}, False, deps.get)
    assert got == [], got

    # No header change: only the changed .cpp files.
    got = select(sources, {"lib/c.cpp"}, set(), False, deps.get)
    assert got == ["lib/c.cpp"], got

    # A .clang-tidy change selects everything.
    got = select(sources, set(), set(), True, deps.get)
    assert got == sources, got

    # A TU whose dependencies cannot be computed is selected, never skipped.
    got = select(sources, set(), {"include/agent/x.h"}, False, lambda s: None)
    assert got == sources, got

    # Paths are matched on the tail, since -MM may print './x' or an absolute path.
    got = select(["lib/a.cpp"], set(), {"include/agent/x.h"}, False,
                 lambda s: ["./include/agent/x.h"])
    assert got == ["lib/a.cpp"], got

    # A multi-line dependency list is parsed, target excluded.
    deps_parsed = parse_deps("lib/a.o: lib/a.cpp include/agent/x.h \\\n  tui/y.h\n")
    assert deps_parsed == {"lib/a.cpp", "include/agent/x.h", "tui/y.h"}, deps_parsed

    print("affected_tus: selftest ok")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--base", help="git ref to diff against, e.g. origin/main")
    ap.add_argument("--cxx", default="c++", help="compiler used for dependency scan")
    ap.add_argument("--cxxflags", default="", help="flags for the dependency scan")
    ap.add_argument("--selftest", action="store_true", help="run the built-in tests")
    ap.add_argument("sources", nargs="*", help="translation units to choose from")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    if not args.sources:
        return 0
    if not args.base:
        ap.error("--base is required (or use --selftest)")

    sources = [normalise(s) for s in args.sources]
    changed = git_changed(args.base)
    if changed is None:
        # Cannot tell what changed: analyse everything rather than nothing.
        print(f"affected_tus: cannot diff against {args.base}; selecting all",
              file=sys.stderr)
        changed = list(sources)

    # LINT_SRCS holds absolute paths while git reports repo-relative ones, so
    # compare (and print) on repo-relative paths throughout.
    root = repo_root()
    rel_of = {normalise(s): normalise(os.path.relpath(os.path.abspath(s), root))
              for s in sources}
    rel_sources = list(rel_of.values())
    abs_of = {rel: os.path.abspath(src) for src, rel in rel_of.items()}

    changed_cpp = {p for p in changed if p in rel_sources}
    changed_headers = {p for p in changed if is_header(p)}
    config_changed = CONFIG_FILE in changed

    # Only a header change needs the dependency scan; a .clang-tidy change
    # selects everything and an unrelated change selects nothing.
    scanned = {}
    if changed_headers and not config_changed:
        deps_of = make_deps_reader(args.cxx, shlex.split(args.cxxflags))
        to_scan = [rel for rel in rel_sources if rel not in changed_cpp]
        with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
            scanned = dict(zip(to_scan, pool.map(
                lambda rel: deps_of(abs_of[rel]), to_scan)))

    for path in select(rel_sources, changed_cpp, changed_headers, config_changed,
                       scanned.get):
        print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
