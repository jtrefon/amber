#!/bin/sh
# Advisory mutation testing on the production lines a branch changed.
#
# Mull instruments the build at compile time and then runs the test program once
# per mutant. This script exists so the same recipe runs locally and in the
# nightly job; it is not a gate and is not part of `make check` or `make verify`.
#
# It is destructive: it reconfigures the tree as an -O0 clang build with Mull's
# frontend and runs `make clean`. Restore afterwards with `./configure && make
# clean && make`, exactly like `make coverage`.
#
# Requirements:
#   - clang++ with the matching Mull frontend (MULL_FRONTEND)
#   - mull-runner (MULL_RUNNER)
#   - a full git history for the base ref (fetch-depth: 0 in CI)
#
# Environment:
#   MUTATION_BASE        base ref (default origin/main; HEAD for the working tree)
#   MUTATION_JOBS        build parallelism (default: nproc)
#   MUTATION_WORKERS     mutant parallelism (default: MUTATION_JOBS)
#   MUTATION_TIMEOUT_MS  baseline/per-mutant budget (default 900000: the
#                        full unit suite is ~3 min, so 30 s would kill it)
#   MUTATION_TEST_ARGS   optional run_tests filter, for iterating on one module
#   MUTATION_REQUIRE_MUTANTS  1 fails when the diff produced no mutants
#   MUTATION_REPORT      SQLite path (default mutation.sqlite)
#   MUTATION_MARKDOWN    summary path (default mutation.md)
#   CXX                  clang++ driver (default clang++)
#   MULL_RUNNER          mull-runner binary
#   MULL_FRONTEND        mull-ir-frontend plugin
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
base=${MUTATION_BASE:-origin/main}
jobs=${MUTATION_JOBS:-$(nproc)}
workers=${MUTATION_WORKERS:-$jobs}
timeout_ms=${MUTATION_TIMEOUT_MS:-900000}
report=${MUTATION_REPORT:-$root/mutation.sqlite}
markdown=${MUTATION_MARKDOWN:-$root/mutation.md}
cxx=${CXX:-clang++}
runner=${MULL_RUNNER:-mull-runner}
frontend=${MULL_FRONTEND:-/usr/lib/mull-ir-frontend}
config=$root/.mutation-mull.yml
test_args=${MUTATION_TEST_ARGS:-}

fail() {
    echo "mutation-check: $*" >&2
    exit 2
}

command -v "$cxx" >/dev/null 2>&1 || fail "clang++ '$cxx' is not installed"
"$cxx" --version 2>/dev/null | grep -q clang ||
    fail "'$cxx' is not clang; Mull's frontend only loads in clang"
command -v "$runner" >/dev/null 2>&1 || fail "mull-runner '$runner' is not installed"
[ -f "$frontend" ] || fail "Mull frontend '$frontend' does not exist"
git -C "$root" rev-parse --verify "$base^{commit}" >/dev/null 2>&1 ||
    fail "base '$base' is not a commit (a shallow clone has no merge-base)"
diff_ref=$(git -C "$root" merge-base "$base" HEAD) ||
    fail "cannot find the merge-base of '$base' and HEAD"
changed=$(git -C "$root" diff --name-only "$diff_ref" -- lib plugins tools |
    grep -E '\.cpp$' || true)
require=""
[ "${MUTATION_REQUIRE_MUTANTS:-0}" = "1" ] && require="--require-mutants"

changed_count=$(printf '%s\n' "$changed" | grep -c . || true)
echo "mutation-check: base $base (merge-base $(git -C "$root" rev-parse --short "$diff_ref")), $changed_count changed production file(s)"
# Only the changed production lines are mutated. A comment or a test change
# therefore produces no mutants; the report says so rather than pretending a
# score.
{
    echo "includePaths:"
    echo "  - '.*(lib|plugins|tools)/.*\\.cpp$'"
    echo "gitProjectRoot: \"$root\""
    echo "gitDiffRef: \"$diff_ref\""
    echo "gitDiffPaths:"
    echo '  - "lib/"'
    echo '  - "plugins/"'
    echo '  - "tools/"'
} >"$config"

echo "mutation-check: configuring an -O0 clang build with Mull"
(
    cd "$root"
    CXX="$cxx" CXXFLAGS="-O0 -g -grecord-command-line -std=c++17 -fpass-plugin=$frontend" \
        ./configure >/tmp/mutation-configure.log 2>&1 ||
        { cat /tmp/mutation-configure.log >&2; exit 2; }
    make clean >/dev/null 2>&1
    make -j"$jobs" "$root/run_tests" \
        "$root/tests/fixtures/mcp_echo" "$root/tests/fixtures/mcp_http" \
        "$root/tests/fixtures/mcp_ignore_sigterm" \
        "$root/tools/plugins/sysinfo/sysinfo-plugin" \
        "$root/tools/plugins/cdp/cdp-plugin" >/tmp/mutation-build.log 2>&1 ||
        { tail -30 /tmp/mutation-build.log >&2; exit 2; }
)

mkdir -p "$(dirname "$report")" "$(dirname "$markdown")"
rm -f "$report"
echo "mutation-check: running the unit suite once per mutant (workers $workers)"
(
    cd "$root"
    # shellcheck disable=SC2086 # test_args is a deliberate, documented filter
    MULL_CONFIG="$config" "$runner" --workers "$workers" --timeout "$timeout_ms" \
        --reporters SQLite --report-dir "$(dirname "$report")" \
        --report-name "$(basename "$report" .sqlite)" --allow-surviving \
        ./run_tests $test_args
) || fail "mull-runner failed; the report is incomplete"

python3 "$root/tools/mutation_report.py" --report "$report" --markdown "$markdown" \
    --base "$base" --allow-missing $require
echo "mutation-check: the tree is an instrumented build; restore with ./configure && make clean && make"
