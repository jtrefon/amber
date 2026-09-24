# Spec: TUI Architecture (Layers, Ports, Dataflow)

### Purpose

Define the internal architecture of the `tui/` client so that its logic is
testable without a terminal, its I/O is swappable, and its layers cannot leak
into each other. This is the contract the TUI refactoring is measured against:
if the code and this spec disagree, the code is wrong.

Amber is hexagonal at the top level — `lib/` is the UI-free core, `tui/` and
`src/` are clients that only *link* it (`AGENTS.md`). This spec applies the same
discipline **inside** `tui/`, which today is a single undifferentiated layer:
599-line event loop, 37 functions over 50 lines, and eight files at 0% coverage
because their logic is welded to ncurses.

### Ownership

- **This spec governs**: `tui/**`
- **Layer source files** (target):
  - **L1 Domain** — `tui/{command_line,setting_registry,key_binder,key_action,
    input_state,key_read,keys,scroll_dispatch,run_registry,session_browser_core,
    drawer_rows,textutil,rich,palette,path_confine,tool_display,approval_model,
    markdown,markdown_md4c,reasoning_block,window,ui_state,status_bar_layout,
    gauge_text,input_line_layout,completion_context,help_page,option_key_decode,
    form_focus,list_state,dialog_pager,panel_view_text,session_browser_view,
    wrap_columns,cmdline_edit}.{h,cpp}`
  - **L2 Application** — `tui/{event_loop,window_ops,slash_dispatcher,
    agent_event_handlers,command_service,session_service,prompt_service,
    popup_service,help_service,shutdown_service}.{h,cpp}`
  - **L3 Ports** — `tui/{display_port,key_source_port,modal_port,session_port,
    prompt_port,window_ops_port,terminal_port,clock_port}.h`
  - **L4 Adapters** — `tui/{render_engine,event_router,confirm_panel,list_panel,
    panel,panel_view,dialog,form_edit,info_dialog,menu_select,welcome,
    session_controller,feed_manager,tui_window_ops_hooks,tui_ui_services}.{h,cpp}`
  - **L5 Composition** — `tui/{tui,tui_main}.{h,cpp}`
- **Test files**: `tests/tui_tests.cpp` (L1), `tests/session_browser_test.cpp`,
  `tests/command_line_test.cpp`, `tests/completions_test.cpp`, plus new per-unit
  files (e.g. `tests/status_bar_layout_test.cpp`); `tests/tui_pty_test.cpp`
  (end-to-end, real TTY). L2 gets `tests/tui_event_loop_test.cpp` driven by mock
  ports.

---

## Current state (measured on `main` `cb9d054`)

| Observation | Evidence |
|---|---|
| One undifferentiated layer | `tui/*.cpp` mix ncurses calls, agent wiring, session I/O and pure logic in the same files |
| God functions | 37 functions > 50 lines (4,343 lines); `Tui::run()` 599, `register_builtin_actions()` 291 |
| Ports designed but **dead** | `display_port.h`, `modal_port.h`, `session_port.h`, `prompt_port.h` are included by **nobody**; `view.h` / `key_source_port.h` only by tests. Their comments say *"the domain core uses this"* — there is no domain core |
| One port done right | `WindowOpsPort` + `WindowOps` + `TuiWindowOpsHooks` (`tui/window_ops.h`, `tui/tui_window_ops_hooks.h`) — the model to replicate |
| L1 leaks ncurses | `scroll_dispatch.h:5` and `session_browser_core.h:5` include `<ncurses.h>` for key/mouse constants, so "pure" logic cannot compile or be tested without ncurses |
| 0% because of coupling | `form_edit.cpp` (126), `list_panel.cpp` (137), `panel_view.cpp` (85), `info_dialog.cpp` (73), `welcome.cpp` (48), `menu_select.cpp` (6), `tui_ui_services.cpp` (36), `tui_window_ops_hooks.cpp` (26) — all real, referenced features (see the proposal's usage table) |

**Diagnosis.** The intended hexagonal TUI was designed and never built: six port
headers exist, the loop ignores them. This spec finishes that design.

---

## Target architecture

Five layers. Dependencies point **downward only** (L5 → L4 → L3 → L2 → L1), and
L2 reaches L4 **only through L3**.

| Layer | Responsibility | May include | Must NOT include | Testable |
|---|---|---|---|---|
| **L1 Domain** | UI state + pure transforms (input editing, completion, layout maths, view-model assembly, widget state machines) | std, `lib/` value types, other L1 | ncurses, any `*_port.h`, any L4/L5 header | **Yes** — plain unit tests, no TTY |
| **L2 Application** | Use cases: drive the loop, translate intents/events into state changes via ports | L1, L3, `lib/` | ncurses, L4 headers, L5 | **Yes** — mock ports |
| **L3 Ports** | Interfaces only (the hexagon boundary) | std, L1 value types | ncurses, any impl, L2/L4/L5 | n/a |
| **L4 Adapters** | ncurses + concrete I/O; translate to/from ports | ncurses, L3, L1 types | L2 (direction is wired by L5), L5 | No (thin by design) |
| **L5 Composition** | Construct adapters, inject ports into L2, own lifetimes, wire direction | everything | — | n/a |

### Ports inventory

| Port | Status | Implemented by (target) |
|---|---|---|
| `WindowOpsPort` | ✅ **wired** | `TuiWindowOpsHooks` |
| `DisplayPort` | 🔧 designed, dead | `RenderEngine` (+ `NcursesDisplay`) |
| `KeySourcePort` | 🔧 designed, dead | `NcursesKeySource` (today: inline `getch()`) |
| `ModalPort` | 🔧 designed, dead | `NcursesModals` → `form_edit`/`info_dialog`/`menu_select`/`list_panel` |
| `SessionPort` | 🔧 designed, dead | `SessionController` |
| `PromptPort` | 🔧 designed, dead | `RunRegistry` + `EventRouter` |
| `View` | 🗑 retire | superseded by the four above; keep only as a test double or delete |
| `TerminalPort` (new) | ➕ to add | `NcursesTerminal` (`getmaxyx`) |
| `ClockPort` (new) | ➕ to add | steady-clock adapter (makes tick logic deterministic in tests) |
| `agent::UiServices` | ✅ wired | `TuiUiServices` (the plugin-facing port, already correct) |

### Patterns deployed (and where)

| Pattern | Applied to | Why |
|---|---|---|
| **Ports & Adapters (Hexagonal)** | L2 ↔ L3 ↔ L4 | the whole point: swap ncurses for a mock in tests |
| **Composition Root** | `Tui` (L5), `tui_main.cpp` | one place constructs and wires; nothing else news up an adapter |
| **Facade** | `Tui` | keeps the public surface small; the FIX-022/023 intent, finished |
| **Mediator** | `EventRouter` (L4 driving) | workers never touch UI state; they publish |
| **Observer** | `AgentHooks`, `EventBus` | agent → UI events (existing) |
| **Command** | `ActionRegistry` + per-domain registration | slash dispatch stays JSON-driven (`completions.json` is the source) |
| **State** | `UiState`, `FormFocus`, `ListState`, `DialogPager` | replaces ~20 loop locals and widget focus/scroll flags with explicit, testable machines |
| **Strategy** | `DisplayPort` impls, glyph sets (`text::glyph`), `KeyBinder` | swap render/input policy without touching callers |
| **Registry** | `SettingRegistry`, `PluginRegistry`, `ActionRegistry` | lookups (existing) |
| **Builder** | `StatusBarLayout`, `HelpPage::build`, `PanelViewText` | construct a plan/view-model, then paint it |
| **View-Model (MVVM-lite)** | `Canvas`, `rich::Line`, `RenderEngine` assembly | view-model assembly is L1 and pure; painting is L4 |

### Dataflow (unidirectional)

```
INPUT (driving)          terminal ──► NcursesKeySource ──► KeySourcePort
                                                              │
                          EventLoop (L2) ──► KeyBinder / CommandLine (L1, pure)
                                                              │
                                                          Intent
                                                              ▼
                       CommandService / PromptService / SessionService (L2)
                                                              │
                                                  UiState / Window / RunRegistry (L1)
                                                              │  (sets dirty)
AGENT (driving)      worker ──► AgentHooks(queue) ──► EventRouter (L4)
                                                              │ AgentEvent
                                       AgentEventHandlers (L2) ──► L1 state (sets dirty)

RENDER (read-only)   dirty ──► RenderEngine::draw() ──► view-model (L1: Canvas,
                              rich::Line, StatusBarLayout, InputLineLayout)
                                                              │
                                                    DisplayPort paint ──► flush()

OUTPUT (driven)      Intent ──► PromptPort.send_async | SessionPort.drain
                              | WindowOpsPort hooks ──► agent / session / disk
```

**The invariant that makes it testable:** *state is mutated only in L2, in
response to an intent or an event; the render path is a pure function of state
and never mutates it.* No event handler paints; no painter mutates.

### Threading

- The **UI thread** owns all L1 and L2 state and every port.
- Agent workers **never** touch L1/L2; they publish through `EventRouter`'s
  queue (mutex-guarded) and are drained on the tick.
- `RunRegistry` (L1) owns per-window busy/cancel; cross-thread flags are
  `std::atomic`. L1 holds **no locks** (single-owner contract, as in `lib/`).
- Approvals/asks block the *worker* on a promise, never the UI thread
  (`tui/dialogs.md`, `EL-09`/`EL-10`).

### Isolation rules (enforceable)

1. **L1 has no ncurses.** L1 defines its own input vocabulary — a pure
   `tui/keys.h` (`Key`/`MouseButton` constants, mirroring `View::Key`) — and L4
   translates ncurses codes into it. This removes the `scroll_dispatch.h` /
   `session_browser_core.h` leaks.
2. **L2 depends on ports, not adapters.** `EventLoop` takes
   `DisplayPort&`, `KeySourcePort&`, `ModalPort&`, `SessionPort&`,
   `PromptPort&`, `WindowOpsPort&`, `TerminalPort&`, `ClockPort&`.
3. **Adapters never call use cases.** `RenderEngine` does not call
   `CommandService`; L5 wires the direction.
4. **Enforcement**: extend `tests/build_hygiene.sh` (which already gates P1–P5
   build invariants) with a **layering check**:
   - no file under the L1 set may `#include` `ncurses.h|panel.h|form.h|menu.h`
     or any `*_port.h`;
   - no L2 file may include an ncurses header or an L4 header;
   - every L3 header includes only std / L1 value types.
   Wire it into `make check` so a leak fails CI, exactly like the existing
   "context.h has no mutex" invariant.
5. **One more guard**: compile the L1+L2 unit-test binaries **without**
   `NCURSES_CFLAGS`; if they need it, the layer is leaking.
6. **Test ownership.** Every L1/L2 unit's tests live in that unit's test file.
   When a unit moves, its tests move with it; tests made redundant by a new pure
   core are deleted, not duplicated; retired doubles (e.g. `View`) are removed
   with their call sites. No test may assert on an implementation detail that no
   longer exists.

---

### Contract

| Dimension | Detail |
|---|---|
| **Input** | Terminal keys (via `KeySourcePort`), agent events (via `EventRouter`), timer ticks (via `ClockPort`), plugin posts (via `agent::UiServices`) |
| **Output** | Screen paints (via `DisplayPort`), agent prompts (`PromptPort`), session/disk writes (`SessionPort`), window side-effects (`WindowOpsPort`) |
| **Error states** | Worker exception → `Error` event → status line (`EL` spec); ncurses init failure → exit; modal + approval collision → deferred queue (never a nested dialog) |
| **Invariants** | The unidirectional rule above; `doupdate()` once per tick (`EL`-2); `getch()` timeout ≤ 50 ms (`EL`-1); L1 is ncurses-free; L2 is adapter-free; adapters are thin |
| **Thread safety** | UI thread owns L1/L2/ports; workers publish only; L1 holds no locks; approvals block workers, not the UI |

---

### Scenarios

Architecture scenarios are **dependency-rule tests** (they fail the build, not a
behavioural assertion). Behavioural scenarios live in `tui/event-loop.md`
(EL-01..EL-18) and become L2 tests with mock ports.

#### [ARCH-01] L1 compiles without ncurses
- **Given**: the L1 source set
- **Input**: `make` with `NCURSES_CFLAGS` unset for the L1-only test target
- **Expected**: builds and its tests pass
- **On failure**: a layering leak (an L1 file includes ncurses)

#### [ARCH-02] No L1 file includes a port or ncurses
- **Given**: `tests/build_hygiene.sh` layering check
- **Input**: `make check`
- **Expected**: zero violations
- **On failure**: CI fails with the offending file:line

#### [ARCH-03] L2 runs with mock ports, no TTY
- **Given**: `EventLoop` constructed with mock `DisplayPort`/`KeySourcePort`/`ClockPort`
- **Input**: a scripted key sequence + a scripted agent event
- **Expected**: the same state transitions the real loop produces (EL-04..EL-14)
- **On failure**: L2 reached into an adapter or a global

#### [ARCH-04] Adapters never call use cases
- **Given**: the L4 source set
- **Input**: grep for includes of L2 headers
- **Expected**: none
- **On failure**: inverted dependency

#### [ARCH-05] `Tui` is a composition root, not a logic owner
- **Given**: `tui/tui.cpp`
- **Input**: `awk '/^void Tui::run/,/^}/' | wc -l`
- **Expected**: ≤ 40 lines; no business logic, only wiring + the tick sequence
- **On failure**: logic leaked back into the facade

---

### Cross-references

- **Depends on**: `tui/event-loop.md` (the loop's behavioural contract and its
  EL-01..EL-18 scenarios), `tui/dialogs.md` (modal/promise model),
  `tui/layout-engine.md`, `tui/scroll-system.md`, `display/markdown-parser.md`
- **Depended on by**: `docs/fix-proposal/tui-god-code-decomposition-2026-09-23.md`
  (the migration), `docs/issues.md` (G1..G9)
- **Related**: `docs/fix-proposal/phase2-tui-facade-2026-08-30.md` (FIX-022/023 —
  the component split this spec layers), `docs/spec/plugins/plugin-framework.md`
  (the `agent::UiServices` port the TUI already implements correctly)
- **Test coverage**: `tests/tui_tests.cpp` (L1), `tests/tui_pty_test.cpp` (e2e);
  new: L1 per-unit files, `tests/tui_event_loop_test.cpp` (L2), and the
  `build_hygiene.sh` layering check (ARCH-01..ARCH-05)

---

### Revision history

| Date | Reason |
|------|--------|
| 2026-09-23 | Initial spec — target layers, ports, dataflow, isolation rules |
