# Local CI parity: why pushes fail, and the one command that prevents it

This page exists because a run of eleven pipeline failures over two days had
four causes, and three of them were decidable before the push. The gates are
stricter than the local loop was; this page makes the local loop match them.

Measured over the 200 most recent CI runs (2026-11-16 to 2026-11-18):

| Cause | Failures | Deterministic? | Local signal that existed |
|---|---:|---|---|
| Hermetic wall-time ceiling (`t-07-planning-multi-file`, 113 ms baseline, 763–1012 ms observed) | 4 | no: shared-runner scheduling | none; the ceiling was too tight for its own noise |
| Coverage per-file **branch ratchet** (`lib/prompt_assembler.cpp`, 22 uncovered decisions, baseline 0, slack 3) | 2 | yes | `make coverage`, but the ratchet only *enforced* with `BRANCH_RATCHET_ENFORCE=true` |
| macOS-only pty timing (`a click outside the menu did not close it`) | 1 | no: UI timing on a loaded runner | pty suite, but only on a Mac and only once |
| clang++ warning-as-error (`-Wmismatched-tags`: class vs struct) | 1 | yes | the default build used one compiler; the dual-compiler build was CI-only |
| A cancelled gating job read as red (`ci-gate`) | 2 | no: watchdog / merge-train cancellation | none; a cancelled job is not a code verdict |
| Watchdog infrastructure failure | 1 | no | none |

Two conclusions:

1. **Four of eleven were a gate measuring noise, not code.** Wall time is
   machine-dependent even with a fake LLM; a 2x+500 ms ceiling on a 113 ms
   scenario turns one scheduling slice into a red main. The gate now keeps the
   ratio for long scenarios and adds an absolute `+2000 ms` headroom floor for
   short ones. Tokens and calls remain the precise axes.
2. **The other deterministic failures were locally checkable but not in the
   pre-PR loop.** The loop said `make verify`; the pipeline also builds two
   compilers with `-Werror`, enforces per-file branch coverage, and runs the
   macOS pty suite. Only the last is genuinely CI-only.

The local mirror also had two holes that only macOS exposed, and both made
`make check` red on the development host:

- five `install-deps` gate tests failed because the script expanded empty arrays
  under `set -u`; bash 3.2 (macOS) treats `"${empty[@]}"` as unbound where bash 5
  does not. The script now uses the `${arr[@]+"${arr[@]}"}` idiom, so the gate
  tests pass on both shells.
- seven prompt-library blocks were Linux-only (`readelf`, ELF `.so` naming,
  GCC `-fopt-info-vec`/`-fanalyzer`, GNU `sed`) or tripped `pipefail` through
  `grep -q`'s SIGPIPE. The checker now honours `# platform: linux` and
  `# requires: <tool>` (reported as `skip`, never as `pass`), the portable
  entries use POSIX-safe `sed`/`grep`, and `make check` passes on macOS with 5
  explicit skips instead of 7 failures.

## The one command

```sh
make preflight
```

It runs, in order:

| Step | What it proves | CI job it mirrors |
|---|---|---|
| `make verify` | static gates (including the gate tests and `library-check`), `make test`, `bench-check`, full `lint`, `analyze`, `smoketest` | `static-gates`, `content-gates`, `build-hygiene`, `build-and-test`, `bench-efficiency`, `lint`, `analyze` |
| `make check-compilers` | the whole tree builds under `g++` and `clang++` with `WERROR=1` | `build-and-test (g++)`, `build-and-test (clang++)` |
| `make agent-e2e` | five CLI journeys through the real binary | `build-and-test` (Linux) |
| `make tui-agent-e2e` | the TUI journeys, if the change can reach them | `build-and-test` (Linux) |

`make preflight` is deliberately not part of `make test`: it is slow, it cleans
the tree twice, and it is a pre-push ritual, not a per-commit one. Before each
push, `make verify-quick` is still the fast loop.

### Prerequisites

The analyzers are pinned to what CI runs. On a Homebrew Mac the pinned LLVM is
keg-only, so `clang-tidy`/`clang-format` may not be on PATH even when llvm@18 is
installed:

```sh
export PATH="$(brew --prefix llvm@18)/bin:$PATH"   # clang-tidy 18 / clang-format 18
make doctor      # names any analyzer that still cannot reproduce CI
make preflight
```

When the host cannot match the pin, `make verify-container` runs the same
mirror in the pinned image.

## The rules this adds

### 1. Wall time is a pathology check, not a benchmark

`make bench-check` compares prompt/completion tokens and tool calls at +10%,
and treats a scenario that passed and now fails as a regression. Those are
deterministic. Wall time is reported and compared at
`max(2 × baseline + 500 ms, baseline + 2000 ms)`. A short scenario may run a
second slower on a loaded runner without failing; a real hang, a synchronous
wait, or a filesystem regression still trips it. Do not add a `bench-allow:`
for wall time unless the measured regression is reproducible on an idle host.

### 2. A new decision branch needs a test for the path that did not happen

The per-file branch ratchet starts a new `lib/`, `tools/` or `plugins/` file at
zero uncovered decisions with 3 slack, and only shrinks. Line coverage at 80%
is not enough: a `return` that no test drives is exactly what the ratchet
catches (measured: `lib/prompt_assembler.cpp` had 94.3% added-line coverage and
22 uncovered decisions).

Reproduce it on Linux or in the parity container (it is destructive — gcov
rewrites every object):

```sh
# On a Linux host, or in a disposable clone that has the pinned toolchain:
BRANCH_RATCHET_ENFORCE=true make coverage BASE=origin/main
```

`make coverage` replaces every object with an instrumented one, so run it in a
disposable checkout or restore with `./configure && make clean && make`. From
macOS without a Linux host, use the parity image (`make parity-image`) with the
repository mounted; the gate itself is CI-authoritative.

If the uncovered branch is genuinely unreachable, that is a design finding:
remove the branch, or record the deliberate exception with
`make coverage-branch-baseline` and say why in the PR.

### 3. Both compilers build with warnings as errors

CI runs the matrix under `g++` and `clang++`; the default local build uses one.
A warning one compiler emits and the other does not is a CI-only surprise
(measured: `-Wmismatched-tags` on `class Config` vs `struct Config`). Run
`make check-compilers` before the PR, or `make verify-container` when the host's
clang is not the runner's version.

### 4. A cancelled job is not a code verdict

`ci-gate` treats `cancelled` as a failure because a cancelled gate measured
nothing. That is correct for the run, but it is not evidence about the commit:
the watchdog and the merge train cancel superseded runs. Before debugging a red
`ci-gate`, check whether the run was cancelled and whether a newer run exists
for the same branch and SHA (`gh run list --branch <branch> --limit 5`). Fix
the newer run, not the cancelled one.

## What is genuinely CI-only

| Check | Why it cannot run locally | Closest local substitute |
|---|---|---|
| Coverage project/patch/branch gates | needs a gcov-instrumented rebuild with CI's gcc and gcovr 8.6; destructive | `make coverage BASE=origin/main` on Linux, or the parity container |
| macOS pty suite | needs the macOS ncurses/terminal stack | `make tui-agent-e2e REPEAT=2` on a Mac; the Linux pty suite still runs |
| Fuzzing | libFuzzer with a clang that ships it; Apple's does not | `make fuzz` on Linux; the seeds are checked in |
| Website audit | npm advisory database moves between runs | `cd website && npm run audit` |
| Wall-time pathology | shared-runner scheduling | the floored ceiling plus the reported ms on the run page |

## Before the PR

- [ ] `make preflight` is clean on the final commit.
- [ ] `make coverage BASE=origin/main` (or the container) is clean for a
      change in `lib/`, `tools/` or `plugins/`; new decision branches are
      exercised on both paths.
- [ ] `make tui-agent-e2e REPEAT=2` for a TUI change; the pty suite was run
      twice, not once.
- [ ] `cd website && npm run audit` if the site or its dependencies changed.
- [ ] The PR body carries the manual end-to-end result (AGENTS.md requires it).
- [ ] A `bench-allow: <reason>` is only for a reviewed change whose overhead is
      intended; it is not a way to quiet a flake.

## Reading a failed job

| Job | First question |
|---|---|
| `complexity`, `nesting`, `class-size`, `debt`, `detach`, `hygiene` | did you run `make verify-quick` on the final commit? |
| `bench-efficiency` | is the violation tokens/calls, or wall time? Only the first two are precise. |
| `coverage` | patch line/branch bar, or the per-file branch ratchet? |
| `build-and-test (clang++)` / `(g++)` | did `make check-compilers` pass locally? |
| `build-and-test-macos` | is the failing test pty/UI timing? Re-run with `--repeat`. |
| `ci-gate` | were any gating jobs *skipped* or *cancelled*? Read the run, not the summary. |
| `website-build` | `npm run audit` before blaming the docs build. |
