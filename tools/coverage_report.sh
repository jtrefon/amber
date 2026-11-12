#!/bin/sh
# Generate the coverage reports and run every coverage gate.
#
# One recipe, called by CI's `coverage` job and by `make coverage`. Keeping the
# gcovr filters, the project floor, the TUI floor, the patch line/branch gates
# and the branch ratchet in one place is the point: a local run that measures a
# different scope than CI is how "coverage is 84%" and "patch coverage failed"
# both get to be true.
#
# Run from the repository root after `make test` on an instrumented build:
#   CXXFLAGS="-O0 -g --coverage" make
#   make test
#   BASE=origin/main tools/coverage_report.sh
#
# Environment:
#   BASE                     required; the patch gate diffs against it
#   PATCH_COVERAGE_ENFORCE   true (default) fails below the bar; false reports
#   BRANCH_RATCHET_ENFORCE   true (default) fails on branch-count growth; false
#                            reports. `make coverage` sets false: the baseline is
#                            measured with CI's pinned gcc, and a different local
#                            compiler shifts branch counts without a code change.
set -eu

BASE="${BASE:-}"
ENFORCE="${PATCH_COVERAGE_ENFORCE:-true}"
BRANCH_RATCHET_ENFORCE="${BRANCH_RATCHET_ENFORCE:-true}"

if [ -z "$BASE" ] || [ "$BASE" = "0000000000000000000000000000000000000000" ]; then
    BASE="$(git rev-parse HEAD~1 2>/dev/null || echo '')"
fi
if [ -z "$BASE" ]; then
    echo "coverage: BASE is required for the patch gate" >&2
    exit 2
fi

# conftest.gcno is left by configure's compile probes (built with the coverage
# CXXFLAGS) and has no surviving source: remove it or gcovr aborts on the
# orphan note file.
rm -rf conftest*

# The project floor covers the unit-testable core. The UI, benchmark harness and
# client entry points are driven by the pty/e2e and bench harnesses, so they are
# reported but not held to a whole-tree floor.
gcovr --root . \
    --gcov-ignore-parse-errors=negative_hits.warn_once_per_file \
    --exclude-throw-branches --exclude-unreachable-branches \
    --filter 'lib/.*' --filter 'tools/.*' --filter 'plugins/.*' \
    --exclude '.*tests/.*' --exclude '.*third_party/.*' \
    --fail-under-line 80 \
    --print-summary

# The patch gate reads Cobertura XML: gcovr's --json-summary entries have no
# `lines` key, so a JSON input would measure nothing and fail closed. Throw and
# unreachable branches are excluded here because they are C++ plumbing, not
# decisions; the gate additionally keeps only decision source lines.
gcovr --root . \
    --gcov-ignore-parse-errors=negative_hits.warn_once_per_file \
    --exclude-throw-branches --exclude-unreachable-branches \
    --filter 'lib/.*' --filter 'tools/.*' --filter 'tui/.*' \
    --filter 'src/.*' --filter 'bench/.*' --filter 'plugins/.*' \
    --exclude '.*tests/.*' --exclude '.*third_party/.*' \
    --exclude '.*fuzz/.*' \
    --exclude '.*src/main\.cpp' --exclude '.*src/smoketest\.cpp' \
    --exclude '.*bench/main\.cpp' --exclude '.*tui/tui_main\.cpp' \
    --exclude '.*src/update\.cpp' \
    --xml-pretty -o coverage.xml

# Top-level percentages for the inline run summary.
gcovr --root . \
    --gcov-ignore-parse-errors=negative_hits.warn_once_per_file \
    --exclude-throw-branches --exclude-unreachable-branches \
    --filter 'lib/.*' --filter 'tools/.*' --filter 'plugins/.*' \
    --exclude '.*tests/.*' --exclude '.*third_party/.*' \
    --json-summary -o coverage-core.json

# The pty harness is the only thing that exercises tui/. A SIGKILLed session
# never flushes gcov, so tui/ can read 0% on an otherwise green run. 1% is not a
# quality bar; it is the assertion that the harness ran and wrote its counters.
# The summary includes the branch rate: that is the TUI branch report, printed
# but not enforced (the patch gate says the same per changed file).
gcovr --root . \
    --gcov-ignore-parse-errors=negative_hits.warn_once_per_file \
    --exclude-throw-branches --exclude-unreachable-branches \
    --filter 'tui/.*' --exclude '.*third_party/.*' \
    --fail-under-line 1 --print-summary

# The branch ratchet comes from the same Cobertura report the patch gate reads, so
# the baseline and the measurement cannot drift apart. `make coverage` restores a
# normal build afterwards; the baseline is regenerated deliberately, never by a
# green run.
RATCHET_FLAG=""
if [ "$BRANCH_RATCHET_ENFORCE" != "true" ]; then
    RATCHET_FLAG="--branch-ratchet-report-only"
fi
set +e
# --absent-ok mirrors the gcovr --exclude entry-point patterns above. A changed
# source the report deliberately omits is not a measurement gap; the lists must
# stay in step.
python3 tools/coverage_gate.py --report coverage.xml --diff-base "$BASE" \
    --branch-baseline tests/branch_baseline.json $RATCHET_FLAG \
    --absent-ok '.*src/main\.cpp' --absent-ok '.*src/smoketest\.cpp' \
    --absent-ok '.*bench/main\.cpp' --absent-ok '.*tui/tui_main\.cpp' \
    --absent-ok '.*src/update\.cpp' \
    --absent-ok '.*fuzz/.*'
rc=$?
set -e

if [ "$ENFORCE" = "true" ] && [ "$rc" -ne 0 ]; then
    echo "::error::patch line/branch coverage below the bar or branch ratchet (set PATCH_COVERAGE_ENFORCE=false to report without blocking)"
    exit "$rc"
fi
if [ "$ENFORCE" != "true" ]; then
    echo "coverage: patch gate reported rc=$rc with enforcement off"
fi
exit 0
