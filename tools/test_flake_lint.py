#!/usr/bin/env python3
"""Wall-clock assertion lint for the test suite.

A test that asserts `elapsed < 500 ms` is a test that passes on an idle
developer machine and fails on a loaded CI runner. The regression these bounds
guard against is usually a seconds-long wait, so a bound an order of magnitude
below that has all the diagnostic power and none of the stability.

The rule: an assertion with an upper wall-clock bound below 15 s must carry an
inline justification:

    ASSERT(elapsed.count() < 500); // flake-allow: the regression is the 60 s deadline

The marker is deliberate, not a silencer: `--report` prints every acknowledged
bound and its reason, so the allowlist cannot grow invisibly. A polling
`wait_until(predicate, budget)` is not an assertion about elapsed time and is
not flagged; the budget there is a failure deadline, not a performance claim.
"""

import argparse
import os
import re
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SCAN_DIRS = ("tests",)
SKIP_PARTS = ("fixtures", "plugins", "gates", "mocks", "third_party")
SOURCE_SUFFIXES = (".cpp", ".h", ".hpp")

# A scan that read almost nothing must not report a clean tree.
MIN_FILES_SANE = 30

THRESHOLD_MS = 15000
ALLOW = "flake-allow:"

ASSERTION = re.compile(r"\b(ASSERT|ASSERT_TRUE|ASSERT_FALSE|REQUIRE|require)\s*\(")
ALLOW_REASON = re.compile(r"flake-allow:\s*(\S.*)")
# `< 500`, `<= 2s`-style comparisons. The numeric branch is only trusted when
# the statement also mentions a clock or a duration variable.
NUMBER_BOUND = re.compile(r"(?:<|<=)\s*(\d+(?:\.\d+)?)\s*(?:\)|;|,)")
DURATION_BOUND = re.compile(
    r"(?:<|<=)\s*(?:std::chrono::)?(milliseconds|seconds|minutes)\s*\(\s*"
    r"(\d+(?:\.\d+)?)\s*\)"
)
TIME_HINT = re.compile(
    r"elapsed|_ms\b|\bms\b|waited|took|duration|count\(\)|toggled|started|begin|"
    r"frame|latency|wall"
)
UNIT_MS = {"milliseconds": 1, "seconds": 1000, "minutes": 60000}

MAX_STATEMENT_LINES = 40


def statements(source):
    """Yield (assertion_line, statement) for C++ statements containing a `;`.

    The join is intentionally simple: a bound can sit on a later line than the
    assertion, so the unit is the statement, not the physical line. A statement
    that never closes is bounded by MAX_STATEMENT_LINES rather than consuming
    the rest of the file.
    """
    lines = source.splitlines()
    index = 0
    while index < len(lines):
        start = index
        buffer = lines[index]
        while ";" not in buffer and index + 1 < len(lines):
            index += 1
            buffer += "\n" + lines[index]
            if index - start >= MAX_STATEMENT_LINES:
                break
        if ASSERTION.search(buffer):
            match = ASSERTION.search(buffer)
            assertion_line = start + 1 + buffer[: match.start()].count("\n")
            yield assertion_line, buffer
        index += 1


def bound_ms(statement):
    """The smallest upper wall-clock bound in ms, or None."""
    values = []
    for match in DURATION_BOUND.finditer(statement):
        values.append(UNIT_MS[match.group(1)] * float(match.group(2)))
    if TIME_HINT.search(statement):
        for match in NUMBER_BOUND.finditer(statement):
            values.append(float(match.group(1)))
    return min(values) if values else None


def findings(source):
    """(line, statement, ms, reason_or_error) for each unacknowledged bound."""
    out = []
    for line, statement in statements(source):
        ms = bound_ms(statement)
        if ms is None or ms >= THRESHOLD_MS:
            continue
        marker = ALLOW_REASON.search(statement)
        if marker:
            continue
        out.append((line, " ".join(statement.split()), ms))
    return out


def acknowledged(source):
    """(line, ms, reason) for every marker that suppresses a real bound."""
    out = []
    for line, statement in statements(source):
        ms = bound_ms(statement)
        if ms is None or ms >= THRESHOLD_MS:
            continue
        marker = ALLOW_REASON.search(statement)
        if marker:
            out.append((line, ms, marker.group(1).strip()))
    return out


def source_files():
    for directory in SCAN_DIRS:
        for base, _, files in os.walk(os.path.join(REPO_ROOT, directory)):
            for name in sorted(files):
                if not name.endswith(SOURCE_SUFFIXES):
                    continue
                path = os.path.join(base, name)
                rel = os.path.relpath(path, REPO_ROOT)
                if any(part in rel.split(os.sep) for part in SKIP_PARTS):
                    continue
                yield rel, path


def scan():
    """{relative_path: [(line, statement, ms)]} plus the scanned file count."""
    found = {}
    scanned = 0
    for rel, path in source_files():
        scanned += 1
        try:
            with open(path, encoding="utf-8", errors="ignore") as handle:
                hits = findings(handle.read())
        except OSError:
            continue
        if hits:
            found[rel] = hits
    return found, scanned


def check(found, scanned):
    if scanned < MIN_FILES_SANE:
        print(
            f"flake-lint: FAILED CLOSED - scanned only {scanned} file(s), expected at "
            f"least {MIN_FILES_SANE}. Check SCAN_DIRS before trusting this result."
        )
        return 2
    if not found:
        print(f"flake-lint: {scanned} file(s) scanned, 0 short wall-clock bounds (clean)")
        return 0
    total = sum(len(hits) for hits in found.values())
    print(
        f"flake-lint: {total} wall-clock bound(s) below {THRESHOLD_MS // 1000} s without "
        f"'{ALLOW} <reason>':"
    )
    for rel, hits in sorted(found.items()):
        for line, statement, ms in hits:
            print(f"  {rel}:{line}: {ms:g} ms: {statement}")
    print(
        "  Raise the bound, or justify it inline. A bound this tight flakes on a loaded "
        "runner; the reason is what makes the exception reviewable."
    )
    return 1


def report():
    found, scanned = scan()
    total = sum(len(hits) for hits in found.values())
    acknowledgements = []
    for rel, path in source_files():
        try:
            with open(path, encoding="utf-8", errors="ignore") as handle:
                acknowledgements.extend(
                    (rel, line, ms, reason)
                    for line, ms, reason in acknowledged(handle.read())
                )
        except OSError:
            continue
    print(f"files scanned: {scanned}")
    print(f"unacknowledged bounds below {THRESHOLD_MS // 1000} s: {total}")
    print(f"acknowledged bounds: {len(acknowledgements)}")
    for rel, line, ms, reason in sorted(acknowledgements):
        print(f"  {rel}:{line}: {ms:g} ms: {reason}")
    return 0 if not found else 1


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", action="store_true", help="print allowed bounds and their reasons")
    args = parser.parse_args(argv)
    if args.report:
        return report()
    found, scanned = scan()
    return check(found, scanned)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
