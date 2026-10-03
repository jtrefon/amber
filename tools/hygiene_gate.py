#!/usr/bin/env python3
"""C++ hygiene gate (cliff).

AGENTS.md states several rules that nothing was enforcing. These are the ones
that can be checked without a compiler, plus the one that needs one:

  using-namespace   `using namespace` in a header leaks every name it imports
                    into every translation unit that includes that header, which
                    is how an ODR clash starts. Headers only; in a .cpp it is
                    merely bad manners.
  raw-memory        "Never use raw new/delete" (AGENTS.md, Coding standards).
                    Ownership follows RAII, so a bare `new` in project code is a
                    leak or an exception-safety bug waiting to happen.

  self-contained    `--check-self-contained` compiles each header on its own, with
                    the exact flags the build uses (passed in by the Makefile -- it
                    is what knows them, and measuring with different flags turns
                    failures into artefacts). A
                    header that only builds because some unrelated .cpp included
                    something first is a latent break, and when it breaks it
                    breaks a file nobody was editing.

All three are **cliffs**: measured clean before being switched on, so there is no
baseline to maintain — only zero to stay at.

**Escape hatch.** `hygiene-allow: <reason>` on the same line. Suppressions are
counted by `--report`, so an exception stays visible instead of quietly becoming
the norm. That matters here from the first day: the only raw `new` in the tree is
legitimate (Job's constructor is private, so `make_unique` cannot reach it), and
the gate found it — which is the gate working, not a false positive.

**Excluded.** `bench/scenarios/` is code the benchmark *agent* writes and an oracle
scores; it is deliberately not project style, which is why cppcheck and the format
gate skip it too. Vendored headers are excluded because their size and style are
not ours to fix.

Usage:
  tools/hygiene_gate.py --check
  tools/hygiene_gate.py --check-self-contained
  tools/hygiene_gate.py --report
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

import cpp_source

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SCAN_DIRS = ("include", "lib", "tui", "src", "tools", "bench", "plugins")

EXCLUDED_PREFIXES = ("bench/scenarios/", "third_party/", "include/nlohmann/")

SOURCE_SUFFIXES = (".cpp", ".h", ".hpp", ".c", ".cc")

ALLOW = "hygiene-allow:"

# A healthy scan reads hundreds of files; as in the other gates this floor exists
# only to catch a scan that measured nothing.
MIN_FILES_SANE = 100

USING_NAMESPACE = re.compile(r"^\s*using\s+namespace\b")
# `new`/`delete` as words, but not `= delete` (required by the Rule of Five, and
# present on nearly every class here) and not a substring of an identifier.
RAW_NEW = re.compile(r"\bnew\b")
RAW_DELETE = re.compile(r"\bdelete\b")
EQUALS_DELETE = re.compile(r"=\s*delete\b")


def is_excluded(rel):
    return rel.startswith(EXCLUDED_PREFIXES)


def source_files():
    for directory in SCAN_DIRS:
        for base, _, files in os.walk(os.path.join(REPO_ROOT, directory)):
            for name in sorted(files):
                if not name.endswith(SOURCE_SUFFIXES):
                    continue
                path = os.path.join(base, name)
                rel = os.path.relpath(path, REPO_ROOT)
                if is_excluded(rel):
                    continue
                yield rel, path


def acknowledged(line):
    return ALLOW in line


def paired_lines(source):
    """(structural, original) per line.

    The match has to run against structural text, so a `new` in a comment or a
    string literal cannot trip the gate -- but the acknowledgement lives *in* a
    comment, so it is blanked out of the structural line and has to be read from
    the original. strip_noise preserves line structure exactly, so the two zip
    together by index.
    """
    raw = source.split("\n")
    structural = cpp_source.strip_noise(source).split("\n")
    return list(zip(structural, raw))


def count_suppressions(source):
    return sum(1 for line in source.split("\n") if acknowledged(line))


def using_namespace_findings(source):
    """(line_number, text) for each `using namespace` in a header."""
    out = []
    for number, (line, original) in enumerate(paired_lines(source), start=1):
        if USING_NAMESPACE.match(line) and not acknowledged(original):
            out.append((number, original.strip()))
    return out


def raw_memory_findings(source):
    """(line_number, text) for each raw `new`/`delete`, via the structural lexer.

    Comments and string literals are blanked first, so a `new` inside a message or
    a doc comment cannot trip it.
    """
    out = []
    for number, (line, original) in enumerate(paired_lines(source)):
        if acknowledged(original) or EQUALS_DELETE.search(line):
            continue
        if RAW_NEW.search(line) or RAW_DELETE.search(line):
            out.append((number, original.strip()))
    return out


def scan():
    """(findings, suppressions, files scanned)."""
    findings, suppressions = {}, 0
    scanned = 0
    for rel, path in source_files():
        source, _ = cpp_source.read(path)
        if source is None:
            continue
        scanned += 1
        suppressions += count_suppressions(source)
        hits = []
        if rel.endswith((".h", ".hpp")):
            hits += using_namespace_findings(source)
        hits += raw_memory_findings(source)
        if hits:
            findings[rel] = hits
    return findings, suppressions, scanned


def check(findings, suppressions, scanned):
    """`scanned` is required: a scan that read nothing must not look clean."""
    if scanned < MIN_FILES_SANE:
        print(f"hygiene: FAILED CLOSED - scanned only {scanned} file(s), expected at least "
              f"{MIN_FILES_SANE}. Check SCAN_DIRS and EXCLUDED_PREFIXES before trusting this.")
        return 2
    total = sum(len(v) for v in findings.values())
    if not findings:
        print(f"hygiene: {scanned} file(s) scanned, 0 findings "
              f"({suppressions} acknowledged exception(s)) - clean")
        return 0
    print(f"hygiene: {total} finding(s) in {len(findings)} file(s). Fix it, or acknowledge "
          f"it inline with '{ALLOW} <reason>':")
    for rel, hits in sorted(findings.items()):
        for line, text in hits:
            print(f"  {rel}:{line}: {text}")
    return 1


def report(findings, suppressions, scanned):
    print(f"files scanned: {scanned}")
    print(f"findings: {sum(len(v) for v in findings.values())}")
    print(f"acknowledged exceptions: {suppressions}")
    for rel, hits in sorted(findings.items()):
        for line, text in hits:
            print(f"  {rel}:{line}: {text}")


def headers():
    out = []
    for rel, _ in source_files():
        if rel.endswith((".h", ".hpp")):
            out.append(rel)
    return sorted(out)


def check_self_contained(cxx=None, cxxflags=None, verbose=True):
    """Compile each header on its own. Returns (failures, checked)."""
    cxx = cxx or os.environ.get("CXX", "c++")
    flags = cxxflags.split() if isinstance(cxxflags, str) else list(cxxflags or [])
    found = []
    checked = 0
    with tempfile.TemporaryDirectory() as tmp:
        probe = os.path.join(tmp, "probe.cpp")
        for rel in headers():
            with open(probe, "w", encoding="utf-8") as handle:
                handle.write(f'#include "{rel}"\n')
            checked += 1
            proc = subprocess.run(
                [cxx, "-std=c++17", "-fsyntax-only", *flags, probe],
                capture_output=True, text=True)
            if proc.returncode != 0:
                first = next((l for l in proc.stderr.splitlines() if "error:" in l), "")
                found.append((rel, first.strip()))
                if verbose:
                    print(f"  not self-contained: {rel}")
                    if first:
                        print(f"      {first.strip()[:140]}")
    return found, checked


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument("--check", action="store_true", help="gate: no using-namespace, no raw new/delete")
    group.add_argument("--check-self-contained", action="store_true",
                       help="gate: every header compiles on its own (needs the toolchain)")
    group.add_argument("--report", action="store_true", help="list findings and exceptions")
    ap.add_argument("--cxx", default=os.environ.get("CXX", "c++"),
                    help="compiler for the self-containment check")
    ap.add_argument("--cxxflags", default=None,
                    help="the exact compile flags the build uses (passed by the "
                         "Makefile, which is what knows them)")
    args = ap.parse_args()

    if args.check_self_contained:
        print("hygiene: header self-containment (compiles each header alone)...")
        found, checked = check_self_contained(cxx=args.cxx, cxxflags=args.cxxflags)
        if checked < MIN_FILES_SANE:
            print(f"hygiene: FAILED CLOSED - only {checked} header(s) found, expected at "
                  f"least {MIN_FILES_SANE}.", file=sys.stderr)
            return 2
        if found:
            print(f"hygiene: {len(found)} of {checked} header(s) do not compile standalone. "
                  "Each must include what it uses, so a build break lands on the file that "
                  "caused it rather than an unrelated one.")
            return 1
        print(f"  ok: all {checked} headers compile standalone")
        return 0

    findings, suppressions, scanned = scan()
    if args.report:
        report(findings, suppressions, scanned)
        return 0
    return check(findings, suppressions, scanned)


if __name__ == "__main__":
    sys.exit(main())