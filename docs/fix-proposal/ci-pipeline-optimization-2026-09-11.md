# CI Pipeline Optimization Proposal

**Date:** 2026-09-11
**Trigger:** PR #108 CI investigation — lint takes 28 minutes; total pipeline 37 minutes.

## Current State

### Pipeline topology

```
Phase 1 (parallel, no deps):
  lint (28m)         analyze (58s)    check (2m37s)
  complexity (23s)   format-check (17s)  website-build (11s)

Phase 2 (needs: [lint, analyze, check]):
  build-and-test g++ (9m)   build-and-test clang++ (9m)   build-and-test-macos (6.5m)

Phase 3 (needs: [build-and-test]):
  cd   cd-macos
```

**Total wall time: ~37 minutes** (lint 28m serial-gates build 9m).

### Timing breakdown

| Job | Wall time | Runner | Cores |
|-----|----------|--------|-------|
| lint | 27m48s | ubuntu-latest | 4 |
| build-and-test (g++) | 8m57s | ubuntu-latest | 4 |
| build-and-test (clang++) | 8m54s | ubuntu-latest | 4 |
| build-and-test-macos | 6m30s | macos-latest | 3 |
| check | 2m37s | ubuntu-latest | 4 |
| analyze | 58s | ubuntu-latest | 4 |
| complexity | 23s | ubuntu-latest | 4 |
| format-check | 17s | ubuntu-latest | 4 |
| website-build | 11s | ubuntu-latest | 4 |

## Issues Identified

### Critical (pipeline-blocking)

**C1. lint takes 28 minutes — 55% of total wall time**

Root causes (in order of impact):

1. **No `compile_commands.json`** — clang-tidy is invoked as
   `clang-tidy <file> -- $(LINT_CXXFLAGS)` per TU. Each of the 168 TUs
   re-parses all included headers from scratch. 110 headers (8,337 lines)
   matching `HeaderFilterRegex` are re-analyzed 168 times.
   For comparison, cppcheck (which uses `--cppcheck-build-dir` for
   incremental caching) takes 58 seconds on the same codebase.

2. **Tests are linted** — 25 test files, 19,346 lines (37% of the 52,420
   total). The largest: `run_tests.cpp` (6,066), `bench_test.cpp` (2,045),
   `agent_loop_test.cpp` (1,836). Test harness macros (`TEST`, `ASSERT_EQ`)
   trigger clang-tidy noise that is then suppressed — wasted analysis.

3. **4-core parallelism** — `xargs -P$(NPROC)` on `ubuntu-latest` (4 cores).
   168 files / 4 cores = 42 files per core, serially.

4. **No caching** — every CI run re-analyzes all 168 files from scratch.
   No `actions/cache` keyed on file hashes.

5. **clang-tidy 18 from apt** — Ubuntu's default package. Newer versions
   (19+) have significant performance improvements.

**C2. build-and-test is serial-gated by lint**

`build-and-test` has `needs: [lint, analyze, check]`. The 9-minute build
doesn't start until the 28-minute lint finishes. If build-and-test only
needed `[check]` (the fast 2.5m hygiene gate), the build would start at
2.5m and overlap with lint. Total would drop from 37m to 28m — **saving 9
minutes** on every green PR.

The `needs` gate prevents wasting build compute if lint fails. But lint
failures are rare (most PRs don't introduce lint issues), so the tradeoff
favors parallelism.

### Structural (DRY/maintainability)

**S1. Duplicated apt-get install boilerplate — 8 copies**

Every Linux job repeats:
```yaml
sudo rm -f /etc/apt/sources.list.d/google-chrome.list* ...
sudo apt-get update || true
sudo apt-get install -y --no-install-recommends \
  build-essential libcurl4-openssl-dev libncursesw5-dev pkg-config
```
Appears in: lint, analyze, check, complexity, format-check,
build-and-test (g++), build-and-test (clang++), cd. Should be a
reusable composite action or a script in `tools/ci/`.

**S2. Jobs install dependencies they don't need**

- `complexity` installs `build-essential libcurl4-openssl-dev
  libncursesw5-dev pkg-config` but only needs Python + lizard.
- `format-check` installs the same build deps but only needs
  `clang-format`.

**S3. No dependency caching**

No `actions/cache` for:
- apt packages (could cache `/var/cache/apt`)
- Homebrew packages (macOS jobs run `brew install` every time)
- Build objects (no `ccache` — every build compiles from scratch)

### Coverage gaps (inconsistent scanning)

**G1. format-check excludes tests/ and bench/**

`format-check` scans `lib tools tui src include/agent plugins` but
not `tests/` or `bench/`. Test and benchmark code should be formatted
too.

**G2. complexity excludes tests/, bench/, plugins/**

`complexity` scans `lib tools tui src` but not `tests/`, `bench/`,
`plugins/`. Bench code especially should be checked for complexity.

**G3. analyze (cppcheck) excludes bench/, src/, plugins/**

`analyze` scans `lib tools tui tests` but not `bench/`, `src/`,
`plugins/`. Inconsistent with lint which covers all directories.

### Pre-existing compiler warnings (11 unique)

These are not from PR #108 — they exist on main. But they clutter build
output and some indicate real bugs:

| File | Warning | Severity |
|------|---------|----------|
| `bench/probe.cpp:487,530,672` | `!x == 0` — logical not only on LHS | **Bug** — likely `!(x == 0)` intended |
| `lib/dialect_gemini.cpp:217` | unused parameter `stream` | Noise |
| `lib/dialect_gemini.cpp:39` | unused function `text_of_parts` | Dead code |
| `lib/compressor_apply.cpp:70` | `prune_count` set but not used | Dead code |
| `bench/report.cpp:539` | `denied`, `retr` set but not used | Dead code |
| `include/agent/mcp_client.h:121` | unused private field `request_timeout_ms_` | Dead code |
| `include/agent/plugin_runtime.h:187` | unused private field `tools_` | Dead code |
| `tests/run_tests.cpp:4828,4849` | ignoring return value of `chdir()` | Should check |
| `tests/test_util.h:83` | sign-compare in `ASSERT_EQ` macro | Noise |
| `tests/agent_loop_test.cpp:360` | sign-compare | Noise |

### Minor

**M1. macOS openssl warning**

`configure` checks `/usr/include/openssl/ssl.h` which doesn't exist on
modern macOS. The warning is benign (openssl is used indirectly via
libcurl) but noisy. Should check brew's openssl path as a fallback.

**M2. `format-check` and `complexity` are informational-only**

Both targets always print "clean (informational)" regardless of findings.
`format-check` uses `--Werror` but pipes through `head -n 30` which can
mask failures. `complexity` never fails. These should either be real
gates or clearly labeled as non-blocking in CI.

## Proposed Fixes

### Phase 1: Quick wins (high ROI, low risk)

**F1. Exclude tests from lint** — ~37% reduction in lint files

Remove `$(wildcard $(SRC_DIR)/tests/*.cpp)` from `LINT_SRCS` in
`Makefile.in`. Tests are covered by `make test`; linting test harness
macros is wasted analysis.

**F2. Ungate build-and-test from lint** — 9-minute saving

Change `build-and-test` and `build-and-test-macos` `needs` from
`[lint, analyze, check]` to `[analyze, check]`. Lint still blocks the
PR (required check) but build runs in parallel. If lint fails, the PR
is blocked regardless.

**F3. Fix `bench/probe.cpp` logical-not-parentheses** — real bug

`!rep.kpi.recoveries == 0` is almost certainly `!(rep.kpi.recoveries == 0)`
or `rep.kpi.recoveries != 0`. Three occurrences at lines 487, 530, 672.

**F4. Remove dead code (Boy Scout)** — 5 files

- `lib/dialect_gemini.cpp:39` — remove unused `text_of_parts`
- `lib/compressor_apply.cpp:70` — remove or use `prune_count`
- `bench/report.cpp:539` — remove or use `denied`, `retr`
- `include/agent/mcp_client.h:121` — remove `request_timeout_ms_`
- `include/agent/plugin_runtime.h:187` — remove `tools_`

### Phase 2: Structural improvements (medium effort)

**F5. Generate `compile_commands.json`** — major lint speedup

Add `bear` to the CI install step, run `bear -- make` before `make lint`,
and invoke clang-tidy with `-p compile_commands.json`. This lets
clang-tidy reuse the compilation database instead of re-parsing headers
per TU.

**F6. Extract reusable CI install action** — DRY

Create `.github/actions/install-deps/action.yml` composite action that
encapsulates the apt-get/brew install boilerplate. All jobs reference it.

**F7. Remove unnecessary deps from complexity and format-check**

- `complexity`: only install `python3` + `pip install lizard`
- `format-check`: only install `clang-format`

**F8. Add `ccache` to build-and-test** — faster incremental builds

Install `ccache`, cache `~/.ccache` via `actions/cache`, and build with
`CXX="ccache g++"` / `CXX="ccache clang++"`. First run is cold; subsequent
runs skip unchanged TUs.

### Phase 3: Coverage alignment (low effort)

**F9. Add tests/ and bench/ to format-check**

Update the `find` in `format-check` to include `$(SRC_DIR)/tests` and
`$(SRC_DIR)/bench`.

**F10. Add bench/ and plugins/ to complexity**

Update the `lizard` invocation in `complexity` to include
`$(SRC_DIR)/bench` and `$(SRC_DIR)/plugins`.

**F11. Align analyze (cppcheck) source list with lint**

Add `$(SRC_DIR)/bench $(SRC_DIR)/src $(SRC_DIR)/plugins` to the cppcheck
source directories.

### Phase 4: Polish (optional)

**F12. Fix macOS openssl warning**

Update `configure` to check brew's openssl path as a fallback when
`/usr/include/openssl/ssl.h` is not found.

**F13. Upgrade clang-tidy to 19+**

Use LLVM's apt repository (`apt.llvm.org`) instead of Ubuntu's default
for newer clang-tidy with performance improvements.

**F14. Cache clang-tidy results**

Use `actions/cache` keyed on file content hashes to skip unchanged
files across runs.

## Expected Impact

| Fix | Time saved | Effort |
|-----|-----------|--------|
| F1 (exclude tests from lint) | ~10m on lint | 1 line |
| F2 (ungate build from lint) | 9m on pipeline | 2 lines |
| F5 (compile_commands.json) | ~15m on lint | Medium |
| F8 (ccache) | ~5m on build | Medium |
| F3+F4 (fix warnings) | Noise reduction | Low |

**Projected pipeline after F1+F2+F5:**
- Phase 1: lint ~13m (28m - 10m tests - 5m compile_db), build starts at 1m
- Total: max(13m lint, 1m + 9m build) = ~13 minutes (down from 37)

## Implementation Order

1. F3 + F4 (fix warnings + dead code) — Boy Scout, unblocks future
   `-Werror` adoption
2. F1 (exclude tests from lint) — 1 line, immediate 10m saving
3. F2 (ungate build from lint) — 2 lines, 9m saving
4. F9 + F10 + F11 (coverage alignment) — consistency
5. F6 + F7 (DRY install, remove unneeded deps) — maintainability
6. F5 (compile_commands.json) — major lint speedup
7. F8 (ccache) — build acceleration
8. F12 + F13 + F14 (polish) — optional
