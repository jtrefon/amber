# Mutation testing spike — 2026-11-13

Status: decided. Adopted as an **advisory nightly job** with a local command; it
is **not** a gate. This document records the measurement and what would change
the decision.

Tracking issue: #577 (P2, part of #579). Tool: [Mull](https://mull-project.com)
0.38.0, LLVM 18/21 builds.

## Question

Line and branch coverage say code ran, not that the assertions would catch a
wrong value. The gate tools were mutation-tested once, but product code was not.
Is a mutation job viable in this tree, and at what cost?

## Method

- Worktree at `bfaf1d4c` (the `main` tip of the 2026-11 audit), Ubuntu 26.04,
  `clang++-21`, Mull 0.38.0 `Mull-21-…-LLVM-21.1.8` (the CI recipe uses
  clang 18 / Mull 18, the runner's pair).
- Module: `plugins/cache/` — `cache_plugin.cpp`, `cache_reuse.cpp`,
  `cache_text.cpp` (466 lines of production code, 659 including headers), with
  17 tests in `tests/cache_reuse_test.cpp`.
- Instrumented build: `./configure` with
  `CXXFLAGS="-O0 -g -grecord-command-line -std=c++17 -fpass-plugin=<frontend>"`
  and `make run_tests` (plus the MCP/plugin fixtures `make test` builds).
- `mull.yml` limited the frontend to the module:
  `includePaths: ['.*plugins/cache/.*\.cpp$']`.
- Run: `mull-runner --workers 24 --timeout 20000 ./run_tests cache` — the
  module's tests, selected by the suite's positional filter.

## Results

| measurement | value |
|---|---|
| mutants found | 69 |
| killed | 63 |
| timed out | 6 |
| survived | 0 |
| conservative score (killed / total) | 91.3% |
| Mull score (timeouts count as kills) | 100% |
| run time, module tests | 8.0 s (baseline 0.5 s) |
| clean instrumented build | 49 s at 24 cores (`-O0`, plugin on every TU) |
| full unit suite, one run | 1939 passed, 0 failed, 3:06 |
| incremental run, one changed line | 2 mutants, 1.6 s |

**Test selection is the whole story.** The same instrumented binary with only
the `cache_reuse` tests selected scored **37%** (43 survivors), while the
module's full test set scored 91%. A mutation score is therefore a statement
about a test selection, not about the source file alone.

**Two real gaps, both fixed in the same PR** (the tests are in
`tests/cache_reuse_test.cpp`):

- `cache_reuse.cpp` `call_hit_sub_one = hit_calls * 100 < calls`: at exactly one
  hit in 100 calls the rate is 1%, not "below 1%". No test covered the
  boundary, so `<` → `<=` survived. Now covered at 1/100 and 1/101.
- `cache_text.cpp` `below_minimum` uses `input_tokens < minimum`: a prompt
  exactly at the model's cacheable minimum is cacheable, not below it. No test
  sat on the boundary, so `<` → `<=` survived. Now asserted at exactly the
  minimum.

**One equivalent mutant.** `floor_percent`'s `ratio <= 0.0` → `< 0.0` produces
the same output for a zero ratio (both paths return 0). No test can kill it;
a future threshold needs an allowlist or a mutator exclusion for this shape.

**Timeouts are not kills.** Six mutants turned a wait into a non-terminating
condition and hit the 20 s budget. Mull counts them as killed; this report keeps
them separate. Run-to-run counts move by one or two (a timeout can be observed
as a kill or vice versa), which is another reason not to ratchet yet.

## Decision

**Adopt as an advisory nightly job, not a gate.**

- `.github/workflows/mutation.yml` runs nightly against the last 24 hours of
  `main` (and on dispatch with an explicit base). It is a separate workflow, not
  in `ci.yml`, so it cannot block a pull request or change `ci-gate`.
- `make mutation-check` is the same recipe locally. It is destructive (an `-O0`
  clang + Mull rebuild, like `make coverage`); `make mutation-report` summarises
  an existing SQLite report.
- `tools/mutation_check.sh` mutates only the changed C++ lines in `lib/`,
  `plugins/` and `tools/` (`gitDiffRef` + `gitDiffPaths`), so a typical nightly
  diff is a handful of mutants. A diff with no mutable C++ lines (a test, a
  script, a comment) reports 0 mutants and passes; `MUTATION_REQUIRE_MUTANTS=1`
  is the switch a later ratchet would use. The job publishes the score, the
  survivor list and the SQLite report; it never fails on a low score.
- The per-mutant budget defaults to 15 minutes because the full unit suite is
  ~3 minutes and the warm-up run uses the same budget. A 30-second value sounds
  right for a one-module run and kills the full suite's warm-up — measured on
  the first dispatch.
- The workflow installs Mull 0.38.0 from its release, pinned by SHA-256, and
  uses the runner's clang 18 with the matching `mull-ir-frontend-18` plugin.

Why not a gate yet:

1. The suite is one test binary, so Mull discovers one test program and every
   mutant runs all of it. Without per-test coverage the job cannot run only the
   covering tests; module-filtered runs need a hand-maintained source→filter
   map. On a 4-core runner a mutant batch is ~3 minutes, which is fine nightly
   but not as a required check on every PR.
2. The score moves with test selection (37% vs 91% on the same module) and the
   timeout classification moves by one or two between identical runs. A
   threshold without an equivalent-mutant allowlist would be a coin flip.
3. The first threshold would be a number picked to make the current tree pass,
   which is the failure mode the rest of the gate set exists to prevent.

What would change the decision: per-test coverage (or a test binary per
module), which lets Mull run only the covering tests; a stable baseline over
several nightly runs; and an explicit equivalent-mutant list. Then a
conservative-score floor could join `ci.yml` as a gating job.

## Reproduce

```sh
# Local (CI uses clang-18 / mull-18 and the same script):
CXX=clang++ MULL_RUNNER=mull-runner-18 \
MULL_FRONTEND=/usr/lib/mull-ir-frontend-18 \
make mutation-check MUTATION_BASE=origin/main

# The run leaves an instrumented tree:
./configure && make clean && make
```

The script writes `mutation.sqlite` and `mutation.md` (gitignored) and prints
the summary. `MUTATION_TEST_ARGS=cache` narrows the test selection for a quick
iteration; the nightly job runs the whole suite.
