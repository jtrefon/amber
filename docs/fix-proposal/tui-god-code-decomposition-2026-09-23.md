# TUI God-Code Decomposition — Architecture-First Refactoring Strategy

- **Status:** 🟡 Proposed — awaiting sign-off
- **Date:** 2026-09-23
- **Architecture contract:** `docs/spec/tui/architecture.md` (the layers, ports,
  dataflow and isolation rules this strategy implements)
- **Register:** `docs/issues.md` (G1..G9)
- **FIX ids:** FIX-043 (P0 keystone) … FIX-051 (P8). FIX-040..042 are taken
  (coverage scoping, PR #151, merged).
- **Branch:** one branch per FIX; `tui/architecture-keystone` first
- **Prior art:** `docs/fix-proposal/phase2-tui-facade-2026-08-30.md`
  (FIX-022/023 — the component split this layers), `tui-issues-2026-09-10.md`

Measured on `main` at `cb9d054` (2026-09-23). Function lengths are physical
lines; coverage figures are instrumented lines from the CI gcovr recipe. This
proposal covers only what is listed; see **Not in scope**.

---

## Problem

Two facts, and the second is the important one.

**1. The god units.** `tui/` has **37 functions over 50 lines (4,343 lines)**;
`Tui::run()` is **599** (the FIX-022/023 facade target was 60), `tui/tui.h` is
**248** (target <200), and `register_builtin_actions()` is 291. Eight real,
referenced files sit at **0%** coverage because their logic is welded to
ncurses. `AGENTS.md:486` already declares the size limits **NON-CONFORMING**.

**2. The architecture was designed and never built.** Six port headers exist —
`display_port.h`, `key_source_port.h`, `modal_port.h`, `session_port.h`,
`prompt_port.h`, `view.h` — and their comments say *"the domain core uses this
to … without depending on ncurses"*. **There is no domain core.** Four of the six
are included by *nobody*; `view.h`/`key_source_port.h` only by tests. Exactly one
port is real: `WindowOpsPort` → `WindowOps` → `TuiWindowOpsHooks`. Meanwhile
`scroll_dispatch.h:5` and `session_browser_core.h:5` `#include <ncurses.h>` for
key/mouse constants, so "pure" logic cannot even compile without ncurses.

So `tui/` is not a codebase that needs a decomposition invented for it. It is a
codebase with a **designed hexagon whose ports were never wired**, and god
functions that bypass them. Decomposing the functions *without* wiring the
architecture would produce many small ncurses-coupled files — same untestability,
more of it. **The architecture comes first; the extractions then have somewhere
correct to land.**

---

## Strategy

**Architecture first, then extract into layers.** Three moves, in order:

1. **Wire the hexagon** — the ports already exist. Give each one an adapter,
   make `EventLoop` depend on the ports, and enforce the dependency rules so
   they cannot rot again. (`docs/spec/tui/architecture.md` is the contract.)
2. **Introduce the missing layers' vocabulary** — a pure `tui/keys.h` (so L1
   stops including ncurses), `UiState` (so the loop's ~20 locals become one
   testable model), and the L2 services.
3. **Extract each god unit into its layer**, with the pattern the spec assigns
   it. Behaviour-preserving; characterization-first.

The end state is the spec's five layers: L1 Domain (pure, unit-tested) → L2
Application (use cases, mock-port-tested) → L3 Ports → L4 Adapters (thin
ncurses) → L5 Composition (`Tui` as a ≤40-line facade).

### Why this order (and not "extract pure helpers as we go")

| | Helpers-only (rejected) | Architecture-first (this proposal) |
|---|---|---|
| Result | many small files, still ncurses-coupled | layered, ports isolated, adapters thin |
| Testability | only the extracted helpers | **all of L1 + L2**, no TTY |
| The dead ports | stay dead | become the seam |
| The 0% files | stay 0% (logic still welded) | logic moves to L1/L2 and is covered |
| Regression risk | each move is ad hoc | each move is verified against a layer contract |

---

## Phase plan

Each phase is one FIX on one branch. P0 is the keystone; every later phase lands
its units in the layer the spec assigns.

| FIX | Phase | God unit(s) (now) | Target layer | Pattern | Extracted units | Tests |
|---|---|---|---|---|---|---|
| **FIX-043** | **P0 keystone** | `Tui::run()` 599, `tui.h` 248 | L2+L3+L5 | Ports & Adapters, Composition Root, State | `event_loop` (L2), `ui_state` (L1), `keys.h` (L1), 4 adapter shims (L4), `Tui` → facade | `tui_event_loop_test.cpp` (mock ports); layering check |
| **FIX-044** | P1 loop domain | rest of `run()` | L1 | Builder, Strategy | `completion_context`, `help_page`, `option_key_decode`, `input_line_layout` | new L1 unit files |
| **FIX-045** | P2 command | `register_builtin_actions()` 291, `build_settings()` 165, `settings_screen()` 152 | L2 + L1 | Command, Registry | `command_service` + per-domain registration (L2); `settings_tree` (L1) | command_line/completions tests + new |
| **FIX-046** | P3 display | `draw_status_bar()` 172, `draw_input()` 132 | L1 + L4 | Builder, View-Model | `status_bar_layout`, `gauge_text`, `input_line_layout` (L1); `RenderEngine` → `DisplayPort` adapter (L4) | `status_bar_layout_test.cpp` |
| **FIX-047** | P4 markdown | `markdown_md4c.cpp` 648 | L1 | View-Model | `md_normalize`, `md_table`, `md_block` | new L1 unit file |
| **FIX-048** | P5 widgets | `form_edit` 171, `list_panel` 80, `panel_view` 89, `info_dialog` 96, `menu_select` | L1 + L4 | State, Ports & Adapters | `form_focus`, `list_state`, `dialog_pager`, `panel_view_text` (L1); widgets → `ModalPort` adapters (L4) | new L1 unit files (**the 0% → covered win**) |
| **FIX-049** | P6 events | `make_hooks()` 108, `drain_events()` 92 | L2 + L4 | Mediator, Observer | `agent_event_handlers` (L2); `EventRouter` stays the L4 mediator | `tui_event_loop_test.cpp` extensions |
| **FIX-050** | P7 sessions | `session_browser()` 141, `load_session()` 51 | L1 + L4 | Builder, Ports & Adapters | `session_browser_view` (L1); `SessionController` → `SessionPort` adapter (L4) | `session_browser_test.cpp` extensions |
| **FIX-051** | P8 remainder | `index_node` 85, `wrap` 90/85, `CommandLine` 62/61/51, `drawer_rows` 86, `Canvas::render` 53, `tui_main` 165 | L1 + L5 | Builder, Strategy | `wrap_columns`, `cmdline_edit`, `main_args`; retire dead ports | new L1 unit files |

### P0 detail — the keystone (FIX-043)

The one phase that touches `Tui::run()` wholesale. It is **pure wiring + move**,
no behaviour change:

1. **L1 vocabulary**: add `tui/keys.h` (pure `Key`/mouse constants mirroring
   `View::Key`); make `scroll_dispatch.h` and `session_browser_core.h` include it
   instead of `<ncurses.h>`; translate ncurses codes in L4.
2. **L1 state**: add `ui_state.h` — the loop's accumulated state (input fill,
   last status tick, drawer state, scroll mode, quit flag, anim phase) as one
   explicit model.
3. **L2 use case**: add `event_loop.{h,cpp}` holding the tick sequence
   (`poll_signal → housekeeping → read_key → dispatch_intent → handle_result →
   render`), depending only on the ports.
4. **L4 adapters**: `NcursesKeySource` (`KeySourcePort`), `NcursesTerminal`
   (`TerminalPort`), `NcursesDisplay` (`DisplayPort`, delegating to
   `RenderEngine`), `NcursesModals` (`ModalPort`, delegating to the widgets).
5. **L5**: `Tui::run()` constructs the adapters, injects them into `EventLoop`,
   and calls it. Target **≤ 40 lines**.
6. **Guard**: the `tests/build_hygiene.sh` layering check (ARCH-01..ARCH-05).

After P0 the god function is gone, the ports are real, and P1–P8 are mechanical
extractions into a structure that already exists.

---

## Test strategy (characterization-first red → green)

Refactoring has no new behaviour to fail on, so red→green is applied as
**characterization tests**:

1. **Red first, honestly.** Before moving a unit, add the test that pins the
   behaviour it must preserve. Where behaviour is already covered (pty flows),
   the test is a guard; where it is not, it is genuinely new coverage. If the
   code is already correct the test passes on first run — **stated, not dressed
   up as a fix**.
2. **Move.** The move must leave every characterization test green; any
   accidental change turns one red. That is the gate.
3. **L2 gets mock ports.** `EventLoop` is tested against mock
   `DisplayPort`/`KeySourcePort`/`ModalPort`/`ClockPort` using the **existing
   EL-01..EL-18 scenarios** from `docs/spec/tui/event-loop.md` — which is what
   the ports were designed for and never used for.
4. **Architecture guards run in CI**: the layering check (ARCH-01..ARCH-05) plus
   compiling the L1/L2 test binaries **without** `NCURSES_CFLAGS`.
5. **Placement**: per-area files (`tests/status_bar_layout_test.cpp` …) wired
   into the Makefile's test objects, following `tests/command_line_test.cpp`.
6. **Test migration and cleanup — part of "done".** A move is not finished until
   the old tests are reconciled, not merely left passing:
   - tests that referenced the moved code **move** to the owning unit's test file
     (e.g. the `scroll_dispatch`/`session_browser_core` cases migrate to the pure
     input vocabulary when those modules stop including ncurses);
   - tests made **redundant** by a new pure core are **deleted**, not left
     duplicated (two tests asserting one behaviour is debt);
   - **obsolete doubles are removed with their call sites** (e.g. `View` when it
     is retired), and any test-only code that no longer has a production
     counterpart goes with it;
   - no test may be left asserting on an implementation detail that no longer
     exists.
   Each FIX reports the before/after test count and must **not lose a behaviour
   assertion** — the number of asserted behaviours is what is preserved, not the
   number of `TEST` blocks.

**Gates per FIX:** `make test` (unit + e2e + pty), `make check` (layering + P5),
`make lint`, `make analyze`, `make duplicates`,
`make format-check-changed BASE=origin/main`, and the `sanitizers`/`tsan` jobs.
`tui_pty_test` is the behavioural backstop for the loop.

---

## Coverage expectation

The 0% files (537 instrumented lines) are the guaranteed win: their logic moves
into L1/L2 and is covered. P0 alone lifts the loop's pure pieces; P5 covers the
widgets. Each FIX records its own delta (the `coverage` job reports `tui/`).
No fixed target is promised beyond "strictly increasing" — the floor is set per
FIX from the measurement.

---

## Risks and mitigations

- **P0 is a big move.** Highest-risk phase. Mitigation: it is wiring-only, gated
  by `tui_pty_test` + EL scenarios + `make test` under both compilers and the
  sanitizers; land it alone, before any extraction.
- **Over-abstraction.** Do not invent ports with one impl and no test value.
  The port set is fixed by the spec; `View` is retired, not extended.
- **Adapter thinness drift.** A "thin" adapter that grows logic is caught by
  ARCH-04/ARCH-05 and the per-file line budget.
- **L2 ↔ L4 direction.** Adapters must not call use cases; ARCH-04 gates it.
- **Scope creep.** Bound each FIX to its row; the changed-file
  `format-check`/`lint` ratchet keeps the diff honest.
- **Prior art.** FIX-022/023's component split is *kept*; this completes the
  loop decomposition those FIXes specified but did not finish.

---

## Sequencing and definition of done

**Order:** P0 → P1 → P2 → P3 → P4 → P5 → P6 → P7 → P8 (as tabled). P0 lands
alone first.

**Done when:**
- the spec's five layers exist and the dependency rules hold (ARCH-01..05 green);
- every unit in the table is **< 200 LoC**, no function **> 50 lines**;
- `Tui::run()` **≤ 40 lines**, `tui/tui.h` **< 200**;
- every port in the inventory has exactly one adapter and L2 depends only on
  ports;
- L1 + L2 are unit-tested without ncurses; `tui/` coverage is strictly up;
- `AGENTS.md:486` audit table refreshed (no `tui_input.cpp` entry); all gates
  green.

---

## Not in scope

- Any behaviour, feature, prompt or wire-format change.
- `lib/` or `include/agent/` (the core is already clean: `lib/` 85%).
- A rewrite of the ncurses drawing layer.
- Re-litigating the FIX-022/023 facade split (it stays; this layers it).
- New ports beyond the spec's inventory.

---

## How the numbers were produced

- Function lengths: a brace-matching scan over `tui/*.cpp` top-level definitions
  (523 functions; 37 over 50 lines).
- Coverage: the CI recipe — `--coverage` build, `make test`, then `gcovr
  --root . --filter 'tui/.*' --exclude '.*tests/.*'`; `tui/` is 48.1% with the
  FIX-040 pty-capture fix applied.
- Port usage: `grep` for each `*_port.h` include site; the 0% files' "actually
  used by" column is the same method.
