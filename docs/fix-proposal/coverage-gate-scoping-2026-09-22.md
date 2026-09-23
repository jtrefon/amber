# Coverage Gate Scoping — Proposal

- **Status:** 🟢 Landed 2026-09-22 — FIX-040, FIX-041, FIX-042 shipped
- **Date:** 2026-09-22
- **Register:** `docs/issues.md` (CV1..CV3)
- **FIX ids:** FIX-040 (CV1, TUI capture), FIX-041 (CV2, project gate scope), FIX-042 (CV3, patch gate)
- **Branch:** `fix/coverage-gate-scoping`

Findings below were reproduced locally on macOS by building the tree with the
same gcov instrumentation CI uses (`CXXFLAGS="-O0 -g --coverage -std=c++17"`)
and running `make test`, then `gcovr`. The repo working tree was not modified;
the instrumented build lived outside it. Measurements are stated as measured.

---

## Problem

### CV1 — 🟠 High: the TUI's coverage is measured as 0% because the test kills it

`tests/tui_pty_test.cpp` terminates the TUI with `SIGKILL`:

```
void stop() {
    ...
    kill(pid, SIGKILL);          // tests/tui_pty_test.cpp:211
    ...
}
```

gcov writes `.gcda` from an `atexit` handler. `SIGKILL` cannot be caught, so the
TUI process never flushes and every translation unit reachable only through the
real binary reports 0%. These units are exercised on every pty test run — the
tests pass — but the measurement discards the data.

Measured, same instrumented binary, pty session driven until the UI is up:

| unit | via pty test (`SIGKILL`) | clean exit via `/quit` |
|---|---|---|
| `tui/tui_input.cpp` | 0.0% | **14.7%** |
| `tui/tui.cpp` | 0.0% | **27.5%** |

The largest 0% units are the TUI's core loop:

| file | lines | coverage |
|---|---|---|
| `tui/tui_input.cpp` | 2,239 | 0% |
| `tui/tui.cpp` | 814 | 0% |
| `tui/render_engine.cpp` | 607 | 0% |
| `tui/event_router.cpp` | 393 | 0% |
| `tui/tui_session.cpp` | 386 | 0% |

That is ~5,700 lines the pty/e2e suite drives but the report never sees. This is
a **measurement bug**, not a test gap, and it is the single largest reason the
`tui/` directory sits at 26.5%.

Note the TUI's signal guard (`tui/signal_guard.h`) does *not* rescue this: the
guard sets a flag consumed by the event loop, but the loop is blocked in
`getch()`, so `SIGTERM`/`SIGINT` did not produce an exit in measurement either.
The only reliable path observed was the app's own quit command from a clean
prompt.

### CV2 — 🟠 High: the 80% figure is a *new-code* rule, applied as a whole-tree floor

The threshold the PR enforces is documented as a per-change rule, not a project
floor:

- `.github/PULL_REQUEST_TEMPLATE.md:28` — "Tests cover the new behaviour
  (≥80% line coverage for **new code paths**)"
- `docs/fix-tracker.md:494` — "**Coverage threshold**: new code paths must have
  ≥80% line coverage."

`a70da98` added `--fail-under-line 80` across the *entire* production tree
(`lib tools tui src bench plugins`), turning a patch guideline into a
whole-project gate. CI then failed at ~60%. That is the actual mistake: a
project-wide 80% floor was never a stated requirement.

### CV3 — 🟡 Medium: the unblocking change is arbitrary and undocumented

`3f1f8f2` narrowed the filters to `lib/` + `tools/` only:

```diff
-            --filter 'lib/.*' --filter 'tools/.*' --filter 'tui/.*' \
-            --filter 'src/.*' --filter 'bench/.*' --filter 'plugins/.*' \
+            --filter 'lib/.*' --filter 'tools/.*' \
             --exclude '.*tests/.*' --exclude '.*third_party/.*' \
+            --fail-under-line 80 \
```

This silently drops `plugins/` (measured **86.5%**, *better* than the gate it
was excluded from), plus `tui`, `bench` and `src`, with no rationale and no
tracking. It is a filter list tuned to make one run green, which is why it reads
as a hack.

### Measurements (full production surface, CI filter set)

Overall: **61.0%** (15,918 / 26,079 lines) — reproduces the CI number.

| dir | covered | total | % | uncovered |
|---|---|---|---|---|
| `lib/` | 8,831 | 10,534 | 83.8% | 1,703 |
| `tools/` | 1,298 | 1,740 | 74.6% | 442 |
| `plugins/` | 610 | 705 | 86.5% | 95 |
| `bench/` | 2,887 | 4,082 | 70.7% | 1,195 |
| `tui/` | 2,294 | 8,663 | 26.5% | 6,369 |
| `src/` | 0 | 355 | 0% | 355 |

The unit-testable core — `lib` + `tools` + `plugins`, whose designated harness
is the unit suite — is **82.7%** (10,739 / 12,979), already above 80%.

### Why a whole-tree 80% is not the right target

80% of 26,079 is 20,863 lines, i.e. **+4,945** covered lines. `tui/` alone holds
6,369 uncovered lines, dominated by a 2,239-line ncurses input loop and the
render/event-loop glue. Even with CV1 fixed, capturing the pty suite fully moves
`tui/` by only a few points. Reaching a whole-tree 80% would require a very
large, low-value unit-test campaign against terminal rendering — the opposite of
the "high-value tests" the register's tolerance asks for. The industry-standard
answer is not to test the UI harder but to **measure each surface with its own
harness and gate the code the unit suite owns**, while enforcing the documented
80% on *new* code (patch coverage).

---

## Decision

**D1 (FIX-040) — capture the TUI's real coverage.** End each pty session through
the app's own quit path so gcov flushes, instead of `SIGKILL`. Bounded
`SIGKILL` stays only as a last-resort guard so the suite can never hang.

**D2 (FIX-041) — scope the project gate to the unit-testable core.** Gate
`lib/` + `tools/` + `plugins/` at 80% with `--fail-under-line 80`. Report `tui/`,
`bench/` and `src/` to Codecov as informational. The only exclusions are
genuine program entry points (`src/main.cpp`, `bench/main.cpp`, `tui/tui_main.cpp`,
`src/smoketest.cpp`), which are universally excluded and carry no unit-testable
logic; each exclusion is named and justified in the workflow comment.

**D3 (FIX-042) — enforce the documented rule where it applies.** Make patch
coverage (new/changed lines ≥80%) the enforced new-code gate via Codecov's
`patch` status, and keep the project status as a no-regression ratchet
(`target: auto`). This is the rule the PR template and fix-tracker actually
state.

D2 is deliberately *not* the current hack: the current list is `lib`+`tools`
only and drops `plugins` (the best-covered directory); D2 keeps every
unit-testable directory, excludes only entry points, documents why, and keeps
`tui`/`bench`/`src` visible rather than hidden.

---

## Implementation plan (Red → Green)

### FIX-040 — TUI capture (measurement)

1. **Red:** extend `tests/tui_pty_test.cpp` so the suite fails if the TUI's
   coverage is not captured — e.g. a test that asserts `tui/tui_input.cpp` has a
   non-zero `.gcda` hit count after a driven session (guard: only when built
   with `--coverage`). Verify it fails today (0 hits).
2. **Green:** change `Tui::stop()` to drive the prompt to a clean state and send
   the app's quit command, waiting (bounded) for a clean exit; keep `SIGKILL`
   only as the fallback. Validate that all five pty tests still pass and that
   `tui_input.cpp` / `tui.cpp` now report non-zero coverage.
3. Confirm no test hangs on the macOS controlling-terminal reap path (the
   existing comment in `stop()` records that hazard).

### FIX-041 — project gate scope

1. Restore the full filter set in `.github/workflows/ci.yml`, add the named
   entry-point exclusions, keep `--fail-under-line 80` on the scoped core.
2. Comment the workflow with the rationale (unit suite owns `lib`/`tools`/`plugins`;
   `tui`/`bench` have their own harnesses and are reported, not gated).
3. Re-run the gate locally to confirm it passes on the scoped core (82.7% today)
   with margin for the FIX-040 change.

### FIX-042 — patch gate

1. Commit `.codecov.yml` with `project: target: auto` (ratchet) and
   `patch: target: 80%` so new code must meet the documented bar.
2. Confirm Codecov reports both statuses on a test PR.

---

## Verification

- `make test` green under `g++` and `clang++` (unchanged behaviour; FIX-040 only
  touches test teardown).
- Instrumented build + `gcovr` shows `tui/tui_input.cpp` and `tui/tui.cpp`
  non-zero after the pty suite (the CV1 Red→Green evidence).
- The scoped gate passes at ≥80% and the CI `coverage` job is green without any
  arbitrary folder narrowing.
- Codecov shows a `patch` status enforcing 80% on a PR that adds uncovered code.

## Risks and alternatives

- **FIX-040 flakiness:** the quit path must work from every state a test ends in.
  Mitigation: normalise the UI state (cancel modal/drawer) before quitting, and
  keep a bounded `SIGKILL` fallback; if a state resists, add a test-only quit
  hook rather than reverting to `SIGKILL`.
- **Scoped gate seen as hiding `tui`:** mitigated by keeping `tui`/`bench`/`src`
  in the report and on Codecov, and by naming the only exclusions (entry points).
- **Alternative considered:** whole-tree 80% via mass TUI unit tests — rejected
  above (low value, large cost, still likely unreachable).

## Not in scope

- Raising `tui/` unit coverage (a separate, optional workstream once CV1 makes
  the real number visible).
- The `bench/` harness's own coverage.
- Any change to the `llama-turboq` inference service or model evaluation.

---

## Implementation status (landed)

All three FIXes shipped on `fix/coverage-gate-scoping`.

| FIX | commits | change |
|---|---|---|
| FIX-040 (CV1) | `31c6e58` (red), `30079f9` (green) | `Tui::stop()` exits through the app's own quit path (cancel any modal, drain the pty, Ctrl+C, bounded wait) so gcov flushes; `SIGKILL` retained only as a last-resort fallback. `ESCDELAY=25` in the pty child so a lone Escape is delivered promptly. |
| FIX-041 (CV2) | `26bed5e` | Coverage job split: a gated run over `lib` + `tools` + `plugins` (`--fail-under-line 80`) plus an ungated full-surface run (`lib tools tui src bench plugins`) writing `coverage.xml` for Codecov, with named entry-point exclusions and a rationale comment. |
| FIX-042 (CV3) | `1066aed` | `.codecov.yml` enforces `patch.target: 80%` (the documented new-code rule) and keeps `project.target: auto` as a no-regression ratchet. |

Measured effect of FIX-040 on the full production surface (exact repo source,
instrumented build outside the tree):

| dir | before | after |
|---|---|---|
| `tui/` | 26.5% | **48.1%** |
| `lib/` | 83.8% | 85.1% |
| overall | 61.0% | **68.7%** |
| functions | 66.7% | 74.9% |

The gated core (`lib` + `tools` + `plugins`) passes at **83.8%** with headroom.

Along the way the delete-confirmation pty test's decline key was found to be
wrong: `ConfirmPanel` accepts only Tab/Enter/Esc, so the test's `"n"` left the
modal open (a latent test bug, not a product bug). It now uses Enter, which
selects the default "No".

### Verification

- `make test` — green (`rc=0`); the FIX-040 red commit failed as intended
  (`FAIL: 5 TUI session(s) were force-killed`), the green commit makes all five
  pty sessions exit cleanly.
- `make check` — green.
- `make format-check-changed BASE=origin/main` — clean.
- Both modified YAML files parse.
- The gated gcovr run passes at 83.8%; the full-report run generates
  `coverage.xml`.

Caveat: local `make lint` / `make analyze` report findings in pre-existing code
and in Homebrew LLVM's own libc++ headers; no findings were reported for the
changed `tests/tui_pty_test.cpp`.
