# Pipeline parity: make the gates the standard, and the standard the gates

Status: phases 0–3 implemented on `feat/pipeline-parity` (draft PR #504).
Owner: engineering. Scope: CI, gate tooling, `AGENTS.md`. Started: 2026-11-01.

## Problem

The pipeline is a superset of what any documented local command runs, and the
written standards are a superset of what any gate enforces. The two drift in
opposite directions, so a change can meet the stated standard and still fail CI,
or pass CI while violating a rule the standard calls mandatory. The observable
cost is the push → wait → red → fix → push loop.

The gate *implementations* are not the problem. They fail closed, carry
sentinels for "measured nothing", and the gate tools have their own unit suite.
The problem is that the inventory, the local commands and the documentation are
three independent lists.

## Measured evidence (2026-11-01)

### Local commands do not cover the gating jobs

`ci-gate` aggregates **17 gating jobs**. `make check` runs the static umbrella
(smoketest, 14 build-hygiene invariants, class-size, nesting, debt, hygiene,
header self-containment, obfuscation, gate-tool tests, library-check, and a gate
report that runs complexity, nesting, class-size, debt, hygiene, obfuscation and
duplicates). It does **not** run the tests, clang-tidy, cppcheck, format,
coverage, sanitizers, TSan or fuzz. `make test` runs none of the gates. There is
no command that means "CI will pass".

`AGENTS.md` describes `make check` as "a separate, lighter gate (`smoketest` +
`tests/build_hygiene.sh` build invariants)", which understates it by eight
steps, and the PR template's verification list names four commands and omits
eight gating checks.

### The tools cannot always reproduce CI

| tool | local (2026-11-01) | CI | effect |
|---|---|---|---|
| clang-tidy | 21.1.8 | 18.1.3 (`clang-tidy-18`) | different check families |
| cppcheck | 2.19.0 | 2.22.0 | `make analyze` fails closed locally |
| clang-format | 18.1.3 (`clang-format-18` is 18.1.8) | 18.1.3 | documented patch-level disagreement |
| lizard | 1.24.0 | 1.24.0 | matches |
| gcovr | 8.6 | 8.6 | matches |

There is no dev-tool bootstrap, no `make doctor`, and no container target for
the two analyzers that cannot be installed at the pinned version on macOS.

### The standards contain ungated rules

| `AGENTS.md` rule | Gate |
|---|---|
| class ≤200 lines | `class-size` |
| method ≤10 lines | none (only the 40-line/CCN-14 cap) |
| `lib/` must not include `tui/`, `src/`, `tools/` | none (P7 covers TUI L1 only) |
| no `std::thread::detach()` | none (PR checkbox) |
| no SPDX/copyright header, first line functional | none |
| prompts descriptive, never/don't/do-not/must banned | none |
| prompt change proved by benchmark | none |
| red test before green fix | none (1 of the last 100 commits says `(red)`) |
| imperative, scoped commit prefix | none |
| one PR per milestone | none |

The reverse also holds: `library-check` is a gating step inside `make check`
with no entry in `AGENTS.md`, and `make gate-report` claims to run "every gate"
while `GATES_RUN` lists 7 of 17.

### Single-point failures found while reviewing

- `lint` ran `lint-changed` on PRs and a `continue-on-error` full-tree job on
  main. That cannot close the hole: the workflow's `push` trigger is
  `main`/`master` only, so a feature-branch push starts no run and a selector
  miss could only be found after the merge. Phase 1 runs the full tree on the
  PR and removes `lint-full`.
- `tui/` coverage is never asserted > 0. A SIGKILLed pty session can leave it at
  0% with a green run; the Codecov upload is known broken and best-effort.
- `format-check-changed` with an empty `BASE` swallowed a `git` error, printed
  "no C++ changes", and exited 0. It also ignored untracked files.
- `duplicate_detector.py` and `obfuscation_guard.py` have no unit tests, though
  both gate CI.
- A job omitted from `ci-gate.needs` is invisible; `gate_needs_check` only
  judges jobs that appear in `needs`, and no test parses `ci.yml`.
- `website-deploy` needs only `website-build`, so it can deploy while
  `website-smoke` is red; `cd`/`cd-macos` publish without a gate dependency.

### Documentation drift

- "1039-case suite" — `./run_tests --list` reports 1673.
- "0 for all 2422 C++ functions" — the nesting scan reports 4028.
- "0 of 172 failing" headers — 274 headers exist.
- "Tests for behavior changes go in `tests/run_tests.cpp`" — there are 55
  `*_test.cpp` files; the suite is per-area.

Only the architecture audit table is mechanically checked (P5). The rest of the
numbers rot silently.

### Recent CI failures

| run | failed job | step |
|---|---|---|
| 41123092618 | `analyze`, `coverage` | cppcheck, patch coverage |
| 41085630081 | `coverage` | patch coverage |
| 41127455879 (main) | `build-and-test (g++)` | Test |

Patch coverage is the repeated one, and it is measurable only in CI: there is no
`make coverage`. Its output does name the uncovered lines, but only after the
push.

## Design

Three invariants.

1. **One inventory.** Every gating job is described once: dimension, events,
   local command (or an explicit waiver with a reason), baseline, docs anchor.
2. **One local command.** `make verify-quick` mirrors the static gate set;
   `make verify` adds build, tests and the independent analyzer. A gate that
   cannot run locally must be runnable in the parity container.
3. **Docs generated from the inventory.** The `AGENTS.md` table and the PR
   template checklist are views of the inventory, checked for drift.

### Gate manifest (Phase 1)

`tools/gates_manifest.py` (or `ci/gates.json`) becomes the single list. Fields:

```text
job | dimension (always/cpp/web) | events | local | gating | reason | docs
```

Consumers:

- `tools/ci_manifest_check.py` parses `.github/workflows/ci.yml` and fails when
  a job is missing from `ci-gate.needs`, the manifest, or has a gating entry
  with no local command and no `local: unavailable` reason.
- `tools/gate_needs_check.py` reads dimensions/events from the manifest; its
  independent self-test fixture stays independent (the existing design reason).
- `make gate-report` enumerates the manifest instead of `GATES_RUN`, and lists
  gates it cannot run as "not measured here" rather than omitting them.
- `tests/gates/test_ci_manifest.py` covers the parser and the drift cases.

`ci.yml` stays literal YAML; validation is safer than generation because a
generated `needs:` list would be easy to commit stale.

### Local parity (Phase 0, this change)

- `make verify-quick`: build-hygiene invariants, format-check-changed,
  lint-changed, class-size, nesting, debt, hygiene, header self-containment,
  complexity, duplicates, obfuscation, gate-tool tests, library-check.
- `make verify`: `verify-quick` + smoketest + `make test WERROR=1` +
  `make analyze`.
- `make doctor`: compares the analyzers against the CI pins and names the
  mismatch. A newer major is a blocker, not a floor pass.
- `BASE` defaults to the merge-base with `origin/main`; an empty `BASE` is a
  hard error in both incremental gates. Untracked files join the format set.
- `tests/build_hygiene.sh` **P15** pins the `verify-quick`/`verify` command
  list, so a gate cannot be dropped from the local mirror silently.

The instrumented coverage build stays out of `verify`: it rebuilds the tree
with gcov flags and would clobber the build `verify` just used. Phase 2 adds
`make coverage BASE=<ref>` as its own deliberate target.

### Missing standards gates (Phase 3)

Add only after parity, otherwise every new gate increases the round-trip cost.

- Layering: `lib/` and `include/agent/` must not include `tui/`, `src/`,
  `tools/` (except tool interfaces). Cliff at zero.
- Hygiene additions: no `std::thread::detach`; no SPDX/copyright first line.
- Prompt lint, scoped to agent-facing prompts (not the coding library).
- Test-presence: executable changes must touch `tests/` or carry a waiver.
- Commit lint, advisory first: scoped prefix, `Closes #`, a `(red)` test commit
  before code commits. Gate new PRs only after the convention is documented.
- Prompt benchmark: run hermetic `amber-bench` when `prompts/` changes; report
  the KPI delta. Gate only after a regression threshold exists.

## Gap register

| id | gap | severity | phase |
|---|---|---|---|
| PP-01 | no single local command for the gating set | high | 0 |
| PP-02 | analyzers not reproducible locally; no bootstrap | high | 0/2 |
| PP-03 | `AGENTS.md`/PR template omit or misstate gates | high | 0 |
| PP-04 | `BASE` unset makes format gate pass without measuring | high | 0 |
| PP-05 | gate inventory duplicated in `needs`/`DIMENSIONS`/`ALL_JOBS`/docs | high | 1 |
| PP-06 | job omitted from `ci-gate.needs` is never required | high | 1 |
| PP-07 | incremental lint is the CI gate; a selector miss is found only after merge | high | 1 |
| PP-08 | `tui/` coverage can be 0% on a green run | medium | 1 |
| PP-09 | `duplicate_detector`/`obfuscation_guard` untested | medium | 1 |
| PP-10 | gate report covers 7 of 17 gates but claims all | medium | 1 |
| PP-11 | deploy/publish jobs do not depend on the full gate set | medium | 1 |
| PP-12 | coverage gate has no local target | high | 2 |
| PP-13 | no container parity for clang-tidy/cppcheck pins | medium | 2 |
| PP-14 | `check` mixes fast static gates and slow header compilation | medium | 2 |
| PP-15 | layering, detach, SPDX, prompt, TDD, benchmark rules ungated | high | 3 |
| PP-16 | stale counts in `AGENTS.md` | low | 0 |

All PP-01 … PP-16 are implemented on this branch except the benchmark half of
PP-15, which is deliberately manual (see Phase 3).

## Phases and acceptance criteria

### Phase 0 (implemented)

- `make doctor` reports local vs CI pins and exits 1 on a blocker.
- `make verify-quick` and `make verify` exist and are pinned by P15.
- `BASE` cannot be empty in either incremental gate; untracked files are
  format-checked.
- `AGENTS.md`, `CONTRIBUTING.md` and the PR template describe the real command
  set; stale counts are removed or made self-reporting.
- `test_doctor.py` and P15 pass; the existing gate suite stays green.

### Phase 1 (implemented)

- Manifest + `ci_manifest_check` + tests; `ci-gate.needs` validated.
- Full-tree `make lint` on every PR; `lint-full` removed (its main-only safety
  net could not block a merge, because feature-branch pushes run no workflow).
- `tui/` coverage > 0 asserted in the coverage job.
- Tests for `duplicate_detector` and `obfuscation_guard`.
- Gate report enumerates every runnable gate or says "not measured here".
- Deploy jobs depend on the gates that certify their inputs.

### Phase 2 (implemented)

- `make coverage BASE=<ref>` (instrumented rebuild + gcovr + patch gate), and
  `tools/coverage_report.sh` is shared with the CI coverage job, so the filters
  and both floors cannot drift.
- `make verify-container` / `make verify-quick-container` run the pinned image
  in `tools/parity/Dockerfile` (clang-tidy 18, cppcheck 2.22, lizard, gcovr,
  clang-format), so a host that cannot install an analyzer still has a way to
  reproduce CI.
- `check` split into `static-gates` (fast, no build), `content-gates`
  (prompt-library run blocks) and `build-hygiene` (smoketest + invariants +
  header self-containment). `make check` still runs the union locally.

### Phase 3 (implemented)

- Layering and first-line boilerplate are part of `make hygiene` (cliffs at
  zero); `std::thread::detach()` is a ratchet over the five existing sites.
- `make prompt-lint` enforces the sentence-initial form on agent-facing prompts.
- `make test-presence` (CI `test-presence`) requires a production change to
  touch `tests/` or carry `tests-allow: <reason>`.
- `make commit-lint` (CI `commit-hygiene`, `continue-on-error`) reports
  scoped-subject violations and the missing `(red)` signal, advisory until the
  convention is adopted.
- Prompt benchmark: **deliberately manual.** CI's hermetic bench uses a fake
  LLM, which cannot measure prompt behavior, and CI has no live model. A green
  hermetic run would not be evidence about a prompt, so the rule stays a
  review/manual requirement rather than a gate that looks like one.

## Non-goals

- Replacing the existing gate implementations. They are the good part.
- Generating `ci.yml` wholesale. Validation catches drift without making the
  workflow unreadable.
- Gating the 10-line method aspiration. `docs/complexity-burndown.md` records
  why the NLOC axis stopped at 40.
- Adding a whole-tree coverage floor for `tui/`, `bench/` or `src/`. The patch
  rule remains the standard; the new assertion only proves the harness ran.

## Risks

- **A local command that is slower than the loop it replaces.** `verify-quick`
  includes clang-tidy and header self-containment; it is a pre-push command,
  not a per-save hook. `verify` is the pre-review command.
- **A newer analyzer produces findings CI will not.** That is why doctor treats
  a major mismatch as a blocker and why `make verify-container` carries the
  pinned image. Do not
  "fix" findings from an unpinned analyzer without checking which tool CI uses.
- **A manifest becomes a fourth list unless it is consumed.** Every consumer in
  Phase 1 must import it; a manifest that only documents is the same drift with
  a new filename.

## Definition of done

A contributor can answer "will CI pass?" with one command, read why a tool
cannot reproduce CI with one command, and find every rule in the standard
either next to the gate that enforces it or marked explicitly as review-only.
A new gating job cannot be added without appearing in the required check, the
local mirror and the documentation.
