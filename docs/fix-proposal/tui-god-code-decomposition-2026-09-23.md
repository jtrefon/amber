# TUI God-Code Decomposition — Proposal

- **Status:** 🟡 Proposed — awaiting sign-off
- **Date:** 2026-09-23
- **Register:** `docs/issues.md` (G1..G9)
- **FIX ids:** FIX-043 (G1) … FIX-051 (G9). FIX-040..042 are taken (coverage
  scoping, PR #151, merged).
- **Branch:** one branch per FIX; `tui/run-decomposition` first
- **Prior art:** `docs/fix-proposal/phase2-tui-facade-2026-08-30.md`
  (FIX-022/023), `docs/fix-proposal/tui-issues-2026-09-10.md`

Measured on `main` at `cb9d054` (2026-09-23) with the tooling described in
**How the numbers were produced**. Function lengths are physical lines
(`awk '/^<sig>/,/^}/'`-equivalent); coverage figures are instrumented lines
from the CI gcovr recipe. This proposal covers only what is listed; see
**Not in scope**.

---

## Problem

`tui/` carries the repository's largest concentration of over-limit functions.
The repo's own audit already declares this:

- `AGENTS.md:486` — **"Architecture audit (status: NON-CONFORMING on size limits)"**.
- `AGENTS.md:296` — "A method/function should stay **under 10 lines** with
  **minimal branching**"; `docs/fix-tracker.md:533` — "classes ≤200 lines,
  methods ≤10 lines".

### Measured inventory

**37 functions in `tui/` exceed 50 lines; together they are 4,343 lines.** The
top 20:

| unit | site | lines |
|---|---|---:|
| `Tui::run()` | `tui/tui.cpp:414` | **599** |
| `SlashDispatcher::register_builtin_actions()` | `tui/tui_input.cpp:940` | **291** |
| `normalize_markdown()` | `tui/markdown_md4c.cpp:688` | **263** |
| `RenderEngine::draw_status_bar()` | `tui/render_engine.cpp:341` | 172 |
| `form_edit()` | `tui/form_edit.cpp:12` | 171 |
| `main()` | `tui/tui_main.cpp:41` | 165 |
| `SlashDispatcher::build_settings()` | `tui/tui_input.cpp:2633` | 165 |
| `Tui::settings_screen()` | `tui/tui_input.cpp:2412` | 152 |
| `SessionController::session_browser()` | `tui/tui_session.cpp:255` | 141 |
| `RenderEngine::draw_input()` | `tui/render_engine.cpp:528` | 132 |
| `append_styled()` | `tui/markdown_md4c.cpp:316` | 130 |
| `EventRouter::make_hooks()` | `tui/event_router.cpp:45` | 108 |
| `Tui::Tui()` | `tui/tui.cpp:64` | 98 |
| `flush_table()` | `tui/markdown_md4c.cpp:215` | 98 |
| `info_dialog()` | `tui/info_dialog.cpp:11` | 96 |
| `EventRouter::drain_events()` | `tui/event_router.cpp:154` | 92 |
| `text::wrap()` | `tui/textutil.cpp:191` | 90 |
| `panel_view()` | `tui/panel_view.cpp:47` | 89 |
| `leave_block()` | `tui/markdown_md4c.cpp:518` | 87 |
| `drawer_rows()` | `tui/drawer_rows.cpp:64` | 86 |

(…plus `cmd_provider` 85, `SettingRegistry::index_node` 85, `rich::wrap` 85,
`ListPanel::handle_key` 80, `handle_slash` 79, `enter_block` 70,
`SessionBrowserCore::key` 66, `CommandLine::on_char` 62, `cmd_set` 61,
`build_view_without_working` 61, `on_tab` 61, `RenderEngine::draw` 59,
`markdown::hl_runs` 55, `draw_drawer` 54, `Canvas::render` 53,
`load_session` 51, `recompute` 51.)

### G1 — 🔴 `Tui::run()` is a 599-line loop, and it is not new debt

`phase2-tui-facade-2026-08-30.md` (FIX-022/023) extracted `WindowManager`,
`EventRouter`, `RenderEngine`, `SessionController` and set two verification
targets: **`Tui::run` < 60 lines** and **`tui/tui.h` < 200 lines**.

Measured today: the components exist and `Tui` forwards to them, but

- `Tui::run()` is **599 lines** (target 60), and
- `tui/tui.h` is **248 lines** (target <200).

The loop decomposition the facade proposal promised — `poll_signals +
process_input + idle_tick`, each <15 — **never landed**, and `run()` has since
absorbed more (macOS Option-key decoding, the help-page renderer, the popup
builders). This proposal is that work, completed and measured.

`run()` mixes at least twelve responsibilities in one scope:

| # | responsibility | lines |
|---|---|---:|
| 1 | startup: first paint, `detect_server`, `build_settings`, `refresh_completions` | 415–431 |
| 2 | completion-context resolution (`update_completions` lambda) | 439–481 |
| 3 | signal-driven graceful shutdown | 487–513 |
| 4 | per-tick housekeeping (drains, plugin tick, `input_fill`) | 514–524 |
| 5 | idle branch (ERR tick, spinner, clock) | 527–546 |
| 6 | macOS Option-as-text UTF-8 assembly + digit normalisation | 554–583 |
| 7 | Alt+0 panels | 587–592 |
| 8 | KeyBinder hotkey dispatch (Alt+1..9, Ctrl+N, ESC stateful) | 594–672 |
| 9 | Ctrl+C cancel/quit semantics | 677–700 |
| 10 | mouse-wheel scroll | 705–717 |
| 11 | CommandLine key routing (the big switch) | 720–817 |
| 12 | result handling: Dispatch / ShowPopup / ShowHelpPage | 819–992 |

Plus a 20-call-site repetition of
`render_engine_->draw(); render_engine_->draw_input(cl.text(), cl.cursor(), cl.shadow());`.

### G2/G3 — the slash layer registers 456 lines in two functions

`register_builtin_actions()` (291) is a flat sequence of `register_action(...)`
lambdas already grouped by domain comments (core, session, window, job,
provider, `os.files`, `os.system`, `config.get`, `config.set`, plugin, mcp,
model). `build_settings()` (165) builds the `SettingRegistry` tree in one scope.
These are the two largest functions outside `run()`.

### G4–G9 — drawing, markdown, widgets, events, sessions, remainder

- `RenderEngine::draw_status_bar()` (172) fuses **layout maths** (zone widths,
  budget, drop-priority pruning), **gauge formatting**, **spinner animation
  state** and **ncurses painting** in one function. `draw_input()` (132) fuses
  cursor/shadow maths with painting.
- `markdown_md4c.cpp` holds four large pure transforms (`normalize_markdown`
  263, `append_styled` 130, `flush_table` 98, `enter_block` 70, `leave_block`
  87) — pure `string → string`, the cheapest possible thing to test.
- The **widgets are at 0% coverage** (see below): `form_edit`, `info_dialog`,
  `list_panel`, `menu_select`, `panel_view`.
- `EventRouter::make_hooks` (108) / `drain_events` (92);
  `SessionController::session_browser` (141) / `load_session` (51).
- Remainder: `SettingRegistry::index_node` (85), `textutil::wrap` (90),
  `rich::wrap` (85), `CommandLine` internals (62/61/51), `drawer_rows` (86),
  `Canvas::render` (53), `tui_main.cpp main` (165).

### Why this matters (two costs, one cause)

**1. Untestable logic, hidden behind ncurses.** Eight `tui/` files are real,
referenced features that measure **0%** because their logic is welded to
ncurses calls:

| file | instrumented lines | 0% because | actually used by |
|---|---:|---|---|
| `form_edit.cpp` | 126 | ncurses FORMS driver loop | `tui_input.cpp:2347,2391,2467`, `event_router.cpp:310` |
| `list_panel.cpp` | 137 | ncurses menu loop | `tui_input.cpp`, `menu_select.cpp` |
| `panel_view.cpp` | 85 | ncurses window painting | `tui.cpp:1086` |
| `info_dialog.cpp` | 73 | ncurses paging | `tui.cpp:946`, `tui_input.cpp:2221` |
| `welcome.cpp` | 48 | ncurses art raster | `tui.cpp:33,159` |
| `tui_ui_services.cpp` | 36 | — (thin forwarder) | `tui.cpp:105` |
| `menu_select.cpp` | 6 | ncurses menu | `tui.cpp:861,877`, `tui_input.cpp:2510` |
| `tui_window_ops_hooks.cpp` | 26 | — | `tui.cpp` |

These are **not dead code** (each is included/called, verified above) — they are
*untestable as written*. Extracting the pure state machines behind them is what
turns 537 lines from 0% into covered, and is the whole point of this proposal.

**2. Change risk.** A 599-line loop with twelve responsibilities is where a
one-line edit breaks an unrelated flow. The pty suite only drives a handful of
paths, so regressions in the un-driven branches are invisible.

---

## Principle (already proven in this codebase — extend it, do not invent)

The TUI already follows **"pure, ncurses-free core + thin shell"**, with the
pure cores unit-tested directly:

| pure module | test |
|---|---|
| `tui/command_line.h` | `tests/command_line_test.cpp` |
| `tui/setting_registry.h` | `tests/completions_test.cpp` |
| `tui/session_browser_core.h` | `tests/session_browser_test.cpp` |
| `tui/drawer_rows.h` | `tests/tui_tests.cpp` |
| `tui/key_binder.h`, `key_action.h`, `input_state.h`, `key_read.h` | `tests/tui_tests.cpp` |
| `tui/scroll_dispatch.h`, `run_registry.h`, `textutil.h`, `rich.h`, `palette.h`, `path_confine.h`, `tool_display.h`, `approval_model.h` | `tests/tui_tests.cpp` |

`Tui::run()` even carries the note at `tui.cpp:433`: *"CommandLine is pure logic
(no ncurses) and fully tested via e2e tests."* This proposal applies that same
move to the remaining god units.

**Hard constraints (unchanged):**

- `tui/` is never depended on by `lib/` (`AGENTS.md`).
- Slash commands stay JSON-driven: `register_action` closures are handlers for
  tree nodes, **not** hardcoded command paths — splitting the registration into
  per-domain methods preserves that rule exactly.
- Behaviour-preserving: no feature, prompt, or wire-format change.
- No test-only production code; no new test framework; no live service.
- Every new unit **< 200 LoC, target ≤ 150**; new methods **≤ 30 lines**, and
  ≤ 10 where practical.

---

## Decomposition plan

| FIX | god unit(s) | now | target | new pure cores (unit-tested) |
|---|---|---:|---:|---|
| **FIX-043** (G1) | `Tui::run()` | 599 | loop ~40 + 12 methods | `completion_context`, `option_key_decode`, `help_page` |
| **FIX-044** (G2) | `register_builtin_actions()` | 291 | ~10 domain methods <60 each | — (structure only) |
| **FIX-045** (G3) | `build_settings()` 165, `settings_screen()` 152 | 317 | ≤4 methods <80 | `settings_tree` builder |
| **FIX-046** (G4) | `draw_status_bar()` 172, `draw_input()` 132 | 304 | thin painters <40 | `status_bar_layout`, `gauge_text`, `input_line_layout` |
| **FIX-047** (G5) | `markdown_md4c` (263+130+98+70+87) | 648 | ≤6 methods <80 | `md_normalize`, `md_table`, `md_block` |
| **FIX-048** (G6) | widgets at 0%: `form_edit` 171, `list_panel` 80, `panel_view` 89, `info_dialog` 96, `menu_select` | 436 | thin shells <40 | `form_focus`, `list_state`, `dialog_pager`, `panel_view_text` |
| **FIX-049** (G7) | `make_hooks()` 108, `drain_events()` 92 | 200 | ≤5 methods <50 | `hook_wiring`, `event_dispatch` |
| **FIX-050** (G8) | `session_browser()` 141, `load_session()` 51 | 192 | ≤4 methods <60 | `session_browser_view` |
| **FIX-051** (G9) | `index_node` 85, `text/rich::wrap` 90/85, `CommandLine` 62/61/51, `drawer_rows` 86, `Canvas::render` 53, `tui_main` 165 | 667 | <200 each | `wrap_columns`, `cmdline_edit`, `main_args` |

### FIX-043 detail — the loop

Extract, in order (each a private method or a pure module):

| extracted | from | lines | testable |
|---|---|---:|---|
| `CompletionContext::for_input(input, settings)` | 439–481 | ~45 | **pure** |
| `Tui::graceful_shutdown(sig)` | 487–513 | ~27 | no |
| `Tui::tick_housekeeping()` | 514–524 | ~11 | no |
| `Tui::on_idle_tick(cl)` | 527–546 | ~20 | no |
| `option_key_decode::read(...)` + `macos_option_digit` | 554–583 | ~30 | **pure** |
| `Tui::handle_hotkeys(ch, cl)` | 594–672 | ~79 | thin |
| `Tui::handle_ctrl_c(cl)` | 677–700 | ~24 | no |
| `Tui::handle_mouse()` | 705–717 | ~13 | no |
| `Tui::route_edit_key(ch, cl)` | 720–817 | ~98 | thin |
| `Tui::handle_result(result, cl)` | 819–992 | ~60 | thin |
| `Tui::show_reference_popup()` / `show_palette_popup()` | 846–883 | ~40 | thin |
| `HelpPage::build(node, settings)` + `Tui::show_help_page(node)` | 884–981 | ~100 | **pure core** |
| `Tui::redraw_prompt(cl)` (DRYs ~20 call sites) | throughout | ~4 | no |
| `Tui::scroll_unhandled(ch)` | 994–1010 | ~12 | no |

The loop body becomes roughly: `if (signal) shutdown(); tick_housekeeping();
ch = getch(); if (ch == ERR) { on_idle_tick(cl); continue; } if (handle_hotkeys(ch, cl)) continue; …route_edit_key → handle_result…`. Target **≤ 40 lines**.

---

## Test strategy (characterization-first red → green)

Refactoring is not feature work, so the red→green discipline is applied as
**characterization tests**:

1. **Red first, honestly.** Before extracting a unit, add a test that pins the
   behaviour the unit must preserve. Where the behaviour is currently untested,
   that test is genuinely new coverage; where it is already covered (e.g. the
   pty flows), the test is a guard. If the code is already correct the test
   passes on first run — and that is stated, not dressed up as a fix.
2. **Extract.** The extraction must leave every characterization test green.
   Any accidental behaviour change turns a test red — that is the gate.
3. **Test the new pure cores directly** (no ncurses, no TTY):
   - `completion_context`, `option_key_decode`, `help_page`
   - `status_bar_layout`, `gauge_text`, `input_line_layout`
   - `md_normalize`, `md_table`, `md_block`
   - `form_focus`, `list_state`, `dialog_pager`, `panel_view_text`
   - `hook_wiring`, `event_dispatch`, `session_browser_view`, `wrap_columns`
4. **Placement:** per-area files following the existing convention
   (`tests/command_line_test.cpp` → e.g. `tests/status_bar_layout_test.cpp`),
   wired into the Makefile's test objects; `tests/tui_tests.cpp` for anything
   that stays in the TUI's own suite.

**Gates per FIX (all must be green):** `make test` (unit + e2e + pty),
`make check` (P5 audit table refreshed), `make lint`, `make analyze`,
`make duplicates`, `make format-check-changed BASE=origin/main`, and the
`sanitizers`/`tsan` jobs. `tui_pty_test` is the behaviour backstop for the loop.

**Coverage expectation.** The 537 instrumented lines currently at 0% are the
guaranteed win: extracting their pure cores puts a large fraction of them under
test. The pure cores from G1/G4/G5/G9 add more. Each FIX records its own delta
(the `coverage` job's full-surface run already reports `tui/`). No target is
promised here beyond "strictly increasing"; the honest floor is set per FIX from
the measurement.

---

## Risks and mitigations

- **ncurses entanglement.** Some code is genuine glue (`attron`/`mvaddnwstr`
  sequences). Do not force-extract it; extract only the pure decision logic and
  leave the painting thin.
- **Behaviour drift in the loop.** The highest-risk item (G1). Mitigation:
  characterization tests + the real-TTY `tui_pty_test` flows + `make test` under
  both compilers and the sanitizers before any commit.
- **Scope creep.** Bound each FIX to the units in its row; the changed-file
  `format-check`/`lint` ratchet keeps the diff to what was touched.
- **`tui.h` still >200 (248).** Trimming the facade header (moving per-domain
  declarations next to their owning component) is a small follow-up, not a
  blocker for the function-level work.
- **Prior art conflict.** FIX-022/023's facade is *kept*, not relitigated; this
  proposal completes the loop decomposition those FIXes specified but did not
  finish.

---

## Sequencing and definition of done

**Order:** FIX-043 (biggest + pure wins) → FIX-044/045 (slash layer) →
FIX-046 (drawing) → FIX-047 (markdown, cheapest) → FIX-048 (widgets, biggest
coverage win) → FIX-049/050 (events/sessions) → FIX-051 (remainder).

**Done when:** every unit in the table is **< 200 LoC** with no function
**> 50 lines**; the extracted pure cores are unit-tested; `tui/` coverage is
strictly up; the `AGENTS.md:486` audit table is refreshed and no longer lists
`tui_input.cpp`; all gates green.

---

## Not in scope

- Any behaviour, feature, prompt or wire-format change.
- `lib/` or `include/agent/` (the core is already clean: `lib/` 85%).
- A rewrite of the ncurses drawing layer.
- Re-litigating the FIX-022/023 facade split (it stays).
- Test-suite splitting beyond the per-area files a FIX needs.

---

## How the numbers were produced

- Function lengths: a brace-matching scan over `tui/*.cpp` top-level
  definitions (523 functions; 37 over 50 lines).
- Coverage: the CI recipe — `--coverage` build, `make test`, then `gcovr
  --root . --filter 'tui/.*' --exclude '.*tests/.*'`; `tui/` sits at 48.1%
  with the FIX-040 pty-capture fix applied.
- The "actually used by" column is `grep` evidence for each 0% file's
  include/call sites.
