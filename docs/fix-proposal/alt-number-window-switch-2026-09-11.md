# TUI Input Loop — Hexagonal Refactor + Alt+Number Fix + `/window set`

Issue: #106 (`alt+number no longer works`)

## Problem

### 1. The God Method: `Tui::run()` is 443 lines, 14 responsibilities, 95 branches

`Tui::run()` (`tui/tui.cpp:340-783`) is the single largest method in the
codebase. It owns **every** concern of the interactive session:

| Lines | Responsibility |
|-------|---------------|
| 341-364 | Init: render, server detect, settings build, feed refresh, CommandLine |
| 368-411 | Completion context computation (`update_completions` lambda) |
| 418-441 | Signal handling (deferred SIGINT/SIGTERM teardown) |
| 442-444 | Housekeeping (drain events, job timeouts, kilo balance poll) |
| 451-472 | Idle tick (spinner, status bar, pending prompt dispatch) |
| 475-480 | Alt+number (8-bit meta form) |
| 481-484 | Ctrl+N (new window) |
| 487-523 | ESC handling — 5 sub-behaviors |
| 526-537 | Ctrl+C (cancel or save+exit) |
| 542-556 | Mouse wheel |
| 558-612 | CommandLine key routing (the big switch) |
| 614-676 | Dispatch result: slash command, prompt send, @-reference popup |
| 678-768 | Help page rendering (man page, children, choices, ranges) |
| 778-781 | Unhandled keys (duplicate PgUp/PgDn) |

**95 if/switch/case branches.** 46 `continue`/`break`/`return` exit points.
Zero unit tests — a grep of `tests/` for `switch_to|Alt+|0xB1` returns nothing.

### 2. The regression: Alt+number broken by PR #93

PR #72 added `meta(stdscr, TRUE)` + `set_escdelay(25)` to make Alt+digit work.
PR #93 added `mousemask(BUTTON4|BUTTON5)` for mouse-wheel scrolling. Enabling
mouse tracking changes ncurses' `\033`-prefix escape-sequence parser. The
follow-up `getch()` after ESC now returns `ERR` on macOS Terminal.app/iTerm2,
so Alt+digit falls through to the plain-ESC scroll-mode toggle.

The 8-bit meta path (`ch >= 0xB1`) is **dead on macOS** — Terminal.app and
iTerm2 default to 7-bit ESC+digit, not 8-bit meta. So neither Alt+digit path
works on the primary development platform.

### 3. The architectural gap: window operations scattered across 4 files

- `Tui::switch_to` (`tui_session.cpp:480`) — busy check + set_active + clear pending + lazy_load + draw
- `Tui::close_window` (`tui_session.cpp:489`) — busy check + autosave + erase + fix active + clear + draw
- `Tui::new_window` (`tui.cpp:146`) — delegates to WindowManager
- `SlashDispatcher::cmd_window_*` (`tui_input.cpp:1768-1782`) — thin wrappers calling Tui
- Inline in `run()` — Alt+digit and Ctrl+N call `switch_to`/`new_window` directly

No single "window operations" interface. Key bindings and slash commands each
reach into `Tui` directly, duplicating coordination logic. Adding a new trigger
path (plugin, MCP) means another copy.

### 4. No `/window set [N]` command

`completions.json` `window` has `new`/`close`/`list`/`rename` only. No way to
switch windows by number from the command line — the terminal-independent
fallback that would make Alt+number non-critical.

## Root cause

The regression is a **symptom**. The **disease** is the God Method: 443 lines
of untestable, terminal-coupled, multi-responsibility code. Patching Alt+number
in place fixes today's symptom and leaves the same trap for the next PR. The
zero-debt policy requires extracting the God Method into a testable, hexagonal
architecture — not patching it.

## Target architecture: hexagonal (ports & adapters), three tiers

```
┌──────────────────────────────────────────────────────────────────┐
│  TIER 1 — Sealed Domain Core (pure, no ncurses, fully tested)    │
│                                                                  │
│  Value types:                                                    │
│    KeyRead     { int key; optional<int> followup; }              │
│    InputState  { drawer_open, busy, scroll_mode,                 │
│                  window_count, has_pending_prompt }              │
│    KeyAction   { Type type; int arg; string text; }               │
│                                                                  │
│  Pure logic:                                                     │
│    WindowManager     (exists — window collection, pure)          │
│    WindowOps         (NEW — window use cases)                   │
│    KeyBinder         (NEW — key → action, data-driven from JSON) │
│    CompletionProvider (NEW — completion context computation)     │
│    HelpPageBuilder   (NEW — help page content assembly)         │
│    CommandLine       (exists — input line logic, pure)          │
│    scroll_dispatch   (exists — mouse wheel math, pure)           │
│                                                                  │
│  Use case orchestrator:                                          │
│    InputLoop         (NEW — the event loop, port-driven)         │
└──────────────────────────┬───────────────────────────────────────┘
                           │ depends on ports (interfaces)
┌──────────────────────────┴───────────────────────────────────────┐
│  TIER 2 — Ports (abstract interfaces, defined by the domain)     │
│                                                                  │
│    KeySourcePort     read_key() → KeyRead, set_timeout(ms)      │
│    DisplayPort      draw(), draw_input(...), flush(),            │
│                     scroll_mode(), max_scroll(), dirty()          │
│    SessionPort      is_busy(), drain_events(), check_timeouts(), │
│                     poll_balance(), consume_signal(),            │
│                     handle_shutdown()                            │
│    WindowOpsPort    on_switch(), on_close(), redraw()            │
│    PromptPort       send_async(str), queue_prompt(str),          │
│                     take_pending_prompt() → optional<str>         │
│    ModalPort        menu_select(title, items) → int,             │
│                     info_dialog(title, lines), redraw_after_modal│
└──────────────────────────┬───────────────────────────────────────┘
                           │ implemented by adapters
┌──────────────────────────┴───────────────────────────────────────┐
│  TIER 3 — Adapters (ncurses implementations, wired by Tui)        │
│                                                                  │
│    NcursesKeySource   wraps getch/timeout, ESC followup read     │
│    NcursesDisplayPort wraps RenderEngine                         │
│    TuiSessionPort     wraps EventRouter + jobs + kilo_balance    │
│    TuiWindowOpsPort   wraps Tui lazy_load/autosave/redraw hooks  │
│    TuiPromptPort      wraps send_async/pending_prompt            │
│    TuiModalPort       wraps menu_select/info_dialog              │
│                                                                  │
│  Existing facades (unchanged):                                    │
│    RenderEngine, EventRouter, SessionController,                │
│    SlashDispatcher, FeedManager, WindowManager                   │
└──────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────┐
│  COMPOSITION ROOT — Tui (thin)                                   │
│                                                                  │
│  void Tui::run() {                                               │
│      // Tui-specific init (stays here)                           │
│      init_session();  // git_refresh, detect_server, feeds       │
│                                                                  │
│      // Wire adapters → domain (dependency injection)            │
│      auto loop = build_input_loop();  // ~10 lines of wiring     │
│      loop.run();      // delegates to InputLoop                   │
│  }                                                               │
└──────────────────────────────────────────────────────────────────┘
```

### Dependency rules (enforced by include direction)

```
Domain core  →  ports, other domain core, agent core (lib/)
Ports        →  standard library only
Adapters     →  ports, domain core, ncurses, existing facades
Tui          →  everything (composition root — wires adapters to domain)
```

The domain core **never** includes ncurses, RenderEngine, EventRouter, or any
adapter. All dependency arrows point inward. This is the hexagonal contract.

### Design decisions

**1. `WindowOps` — use-case layer, not a God Object**

`WindowManager` stays pure (window collection only). `WindowOps` coordinates
cross-cutting concerns via an injected `WindowOpsPort` (DIP):

```cpp
class WindowOps {
public:
    WindowOps(WindowManager& wm, WindowOpsPort& port);
    WindowOpResult switch_to(size_t idx);
    WindowOpResult new_window(const std::string& title);
    WindowOpResult close_window();
    std::string list_windows() const;
    WindowOpResult rename_window(const std::string& name);
    WindowOpResult set_window(size_t one_based_idx);
private:
    WindowManager& wm_;
    WindowOpsPort& port_;
};
```

Each method is 3-5 lines: validate → mutate WM → call port hooks. ~50 lines
total. Well under the 200-line limit. `WindowOpResult` is `{bool ok; string
msg}` — the single return type for all window operations.

**2. `KeyBinder` — pure, data-driven, returns `KeyAction`**

The KeyBinder maps `(KeyRead, InputState) → KeyAction`. It never calls
`switch_to`, `draw`, or `getch`. Pure function, fully testable:

```cpp
class KeyBinder {
public:
    explicit KeyBinder(const nlohmann::json& bindings);
    KeyAction dispatch(const KeyRead& key, const InputState& state) const;
private:
    // JSON-loaded: "alt+1" → {SwitchWindow, arg=0}
    std::map<std::string, KeyAction> simple_bindings_;
    // Coded: ESC stateful logic (drawer/busy/scroll)
    KeyAction dispatch_esc(const InputState& state) const;
};
```

Simple key→action mappings come from `keybindings.json` (data-driven, OCP-open).
The ESC key's stateful logic (drawer open? busy? scroll toggle?) is coded —
it's too complex for declarative JSON and would need a rules engine. This is
the pragmatic line: data for simple bindings, code for stateful dispatch.

**3. `keybindings.json` — single source of truth for hotkeys**

```json
{
  "alt+1": {"action": "switch_window", "arg": 0},
  "alt+2": {"action": "switch_window", "arg": 1},
  "alt+9": {"action": "switch_window", "arg": 8},
  "ctrl+n": {"action": "new_window"},
  "ctrl+c": {"action": "cancel_or_quit"},
  "ctrl+w": {"action": "delete_word"},
  "alt+b": {"action": "delete_word"},
  "esc+digit": {"action": "switch_window_by_digit"},
  "esc+b": {"action": "delete_word"}
}
```

Adding a new hotkey is a data change, not a code change — same philosophy as
`completions.json` for slash commands. The KeyBinder translates terminal codes
to abstract key names (`0xB1` → `"alt+1"`, `ESC+'3'` → `"alt+3"`).

**4. `KeySourcePort` — terminal I/O isolated in the adapter**

The ESC follow-up read (`timeout(200); getch()`) is terminal I/O. It belongs in
the `NcursesKeySource` adapter, not the domain core:

```cpp
struct KeyRead {
    int key;
    std::optional<int> followup;  // set when key==ESC and a followup arrived
};

class KeySourcePort {
public:
    virtual ~KeySourcePort() = default;
    virtual KeyRead read_key() = 0;
    virtual void set_timeout(int ms) = 0;
};
```

`NcursesKeySource::read_key()` reads the first key; if it's ESC (27), it reads
the followup with a 200ms timeout. Returns `{27, '3'}` or `{27, nullopt}`.
Test mocks return followup directly — no terminal needed.

**5. `InputLoop` — the use case, port-driven**

```cpp
class InputLoop {
public:
    InputLoop(KeySourcePort&, DisplayPort&, SessionPort&, WindowOpsPort&,
              PromptPort&, ModalPort&, WindowOps&, KeyBinder&,
              CompletionProvider&, HelpPageBuilder&, SlashDispatcher&);
    void run();
private:
    void handle_key(const KeyRead&, const InputState&);
    void handle_idle();
    void handle_signal();
    InputState snapshot_state() const;
    // ... port references + domain collaborators
};
```

The loop skeleton: check signal → drain events → housekeeping → read key →
if idle: handle_idle → else: snapshot state → KeyBinder.dispatch → execute
action → render. Each step delegates to a port or domain collaborator. The
loop itself is ~30 lines; each handler is 5-15 lines.

**6. `CompletionProvider` — extracts the `update_completions` lambda**

The 40-line `update_completions` lambda (lines 368-411) becomes a pure class:

```cpp
class CompletionProvider {
public:
    explicit CompletionProvider(SettingRegistry& settings);
    CompletionContext compute(const std::string& input) const;
private:
    SettingRegistry& settings_;
};
```

`CompletionContext` is `{vector<string> names, string prefix}`. Pure, testable.

**7. `HelpPageBuilder` — extracts help page assembly**

The 90-line help page rendering (lines 678-768) splits into:
- `HelpPageBuilder` (pure): assembles `vector<string>` from man text, children,
  choices, ranges. No ncurses.
- `ModalPort::info_dialog()` (adapter): displays the assembled page.

**8. `/window set [N]` via completions.json + WindowOps**

Both the KeyBinder (Alt+3) and SlashDispatcher (`/window set 3`) call
`WindowOps::set_window(3)`. One implementation, two trigger paths. DRY.

`completions.json` gets a `window.set` node with `action: "core.window.set"`.
`SlashDispatcher` registers the action as a pure `(action, arg)` closure.
`refresh_window_feed()` merges `1..count` leaves under `window.set` as feed
leaves (per the AGENTS.md hard rule — dynamic values are feeds, never
hardcoded lists).

### What stays unchanged

- `RenderEngine`, `EventRouter`, `SessionController`, `SlashDispatcher`,
  `FeedManager`, `WindowManager` — existing facades keep their interfaces.
- `CommandLine` — already pure, already tested, moves to domain core conceptually.
- `scroll_dispatch` — already pure, already tested.
- `completions.json` structure — only adds `window.set` node.
- All existing tests — no behavioral change, only structural.

### What `Tui::run()` becomes after extraction

```cpp
void Tui::run() {
    // Tui-specific init (stays — not part of the input loop use case)
    render_engine_->git_refresh();
    render_engine_->draw();
    render_engine_->draw_input("");
    render_engine_->flush();
    detect_server(false);
    build_settings();
    refresh_completions();
    refresh_model_list();
    refresh_policy_feed();
    refresh_job_feed();
    refresh_provider_feed();
    refresh_window_feed();  // NEW

    // Wire adapters → domain (composition root — ~15 lines)
    NcursesKeySource key_source;
    NcursesDisplayPort display(*render_engine_);
    TuiSessionPort session(*this);
    TuiWindowOpsPort win_ops_port(*this);
    TuiPromptPort prompt(*this);
    TuiModalPort modal(*this);

    WindowOps window_ops(*window_manager_, win_ops_port);
    KeyBinder key_binder(load_keybindings());
    CompletionProvider completion_provider(settings_);
    HelpPageBuilder help_builder(settings_);

    InputLoop loop(key_source, display, session, win_ops_port,
                   prompt, modal, window_ops, key_binder,
                   completion_provider, help_builder, *slash_dispatcher_);
    loop.run();
}
```

~25 lines. The 443-line God Method is gone.

## RED tests (written first, must fail)

### KeyBinder (pure, no ncurses) — 8 tests

1. `keybinder_meta_digit_maps_to_switch_window` — `{0xB1, nullopt}` → `SwitchWindow{0}`
2. `keybinder_esc_digit_maps_to_switch_window` — `{27, '3'}` → `SwitchWindow{2}`
3. `keybinder_esc_no_followup_drawer_open` — `{27, nullopt}` + `state{drawer_open=true}` → `CloseDrawer`
4. `keybinder_esc_no_followup_busy` — `{27, nullopt}` + `state{busy=true}` → `Cancel`
5. `keybinder_esc_no_followup_idle` — `{27, nullopt}` + `state{idle}` → `ToggleScrollMode`
6. `keybinder_ctrl_n_maps_to_new_window` — `{14, nullopt}` → `NewWindow`
7. `keybinder_busy_state_blocks_window_switch` — `{0xB1, nullopt}` + `state{busy=true}` → `None`
8. `keybinder_loads_bindings_from_json` — load keybindings.json, Alt+1 → `SwitchWindow{0}`

### WindowOps (with mock port) — 11 tests

9. `windowops_switch_to_valid_index` — 3 windows, `switch_to(1)` → active=1, hooks called
10. `windowops_switch_to_same_index_noop` — `switch_to(active)` → no-op
11. `windowops_switch_to_out_of_range_noop` — `switch_to(99)` → rejected
12. `windowops_close_last_window_rejected` — 1 window → close rejected
13. `windowops_close_window_succeeds` — 2 windows → close, active fixed, hooks called
14. `windowops_new_window` — `new_window("chat")` → count increases, active = new
15. `windowops_set_window_valid` — `set_window(2)` with 3 windows → active=1
16. `windowops_set_window_out_of_range` — `set_window(99)` → rejected with message
17. `windowops_set_window_non_numeric` — `set_window("foo")` → rejected with usage
18. `windowops_list_windows` — 3 windows → `"1:chat 2:work* 3:logs"`
19. `windowops_rename_window` — `rename("new")` → title changed

### CompletionProvider (pure) — 3 tests

20. `completion_provider_slash_input` — `"/win"` → window namespace entries
21. `completion_provider_non_slash_input` — `"hello"` → top-level command names
22. `completion_provider_trailing_space_descends` — `"/window "` → window children

### HelpPageBuilder (pure) — 3 tests

23. `help_page_builder_full_man` — `"window"` → man text + children listing
24. `help_page_builder_leaf_with_choices` — `"set.mode"` → help + choices
25. `help_page_builder_leaf_with_range` — `"set.compression.threshold"` → help + range

### Slash command integration — 4 tests

26. `window_set_slash_command_dispatches` — `"/window set 2"` → `WindowOps::set_window(2)`
27. `window_set_rejects_out_of_range` — `"/window set 99"` → status message, no switch
28. `window_set_rejects_non_numeric` — `"/window set foo"` → usage message
29. `window_feed_lists_open_windows` — 3 windows → `"/window set <Tab>"` lists `1`,`2`,`3`

**29 new tests.** All pure (no ncurses). All fail on current main because the
classes don't exist. This is the testability contract: the entire key-dispatch
and window-management surface goes from **zero** coverage to **29 tests**.

## Scope / files

### New files (domain core — pure, no ncurses)

- `tui/key_action.h` — `KeyAction` value type (sum type + args)
- `tui/input_state.h` — `InputState` value type
- `tui/key_read.h` — `KeyRead` value type
- `tui/key_binder.h` / `tui/key_binder.cpp` — pure key→action mapping
- `tui/window_ops.h` / `tui/window_ops.cpp` — window use cases
- `tui/input_loop.h` / `tui/input_loop.cpp` — event loop use case
- `tui/completion_provider.h` / `tui/completion_provider.cpp` — completion context
- `tui/help_page_builder.h` / `tui/help_page_builder.cpp` — help page assembly

### New files (ports — abstract interfaces)

- `tui/key_source_port.h`
- `tui/display_port.h`
- `tui/session_port.h`
- `tui/window_ops_port.h`
- `tui/prompt_port.h`
- `tui/modal_port.h`

### New files (adapters — ncurses implementations)

- `tui/ncurses_key_source.h` / `tui/ncurses_key_source.cpp`
- `tui/ncurses_display_port.h` / `tui/ncurses_display_port.cpp`
- `tui/tui_session_port.h` / `tui/tui_session_port.cpp`
- `tui/tui_window_ops_port.h` / `tui/tui_window_ops_port.cpp`
- `tui/tui_prompt_port.h` / `tui/tui_prompt_port.cpp`
- `tui/tui_modal_port.h` / `tui/tui_modal_port.cpp`

### New data files

- `keybindings.json` — hotkey → action map (single source of truth for keys)

### Modified files

- `completions.json` — add `window.set` node; update `window` man text
- `tui/tui.h` — remove `switch_to`/`close_window`/`new_window` (move to WindowOps);
  remove the `run()` God Method; add `build_input_loop()` wiring
- `tui/tui.cpp` — `run()` becomes ~25 lines (init + wire + delegate)
- `tui/tui_session.cpp` — remove `switch_to`/`close_window` (moved to WindowOps)
- `tui/tui_input.cpp` — register `core.window.set`; add `cmd_window_set`;
  add `refresh_window_feed`; update `core.window` usage leaf
- `tui/slash_dispatcher.h` — declare `cmd_window_set`, `refresh_window_feed`
- `Makefile.in` — add new `.o` targets to `TUI_OBJS` and `run_tests` deps
- `tests/tui_tests.cpp` — 29 new tests
- `docs/fix-tracker.md` — add tracker entry
- `AGENTS.md` — update audit line counts for touched files

### Unchanged

- `tui/render_engine.*` — rendering stays
- `tui/event_router.*` — event routing stays
- `tui/session_controller.*` — session persistence stays
- `tui/window_manager.*` — window collection stays pure
- `tui/command_line.*` — input line logic stays pure
- `tui/scroll_dispatch.*` — mouse wheel math stays pure
- All existing tests — no behavioral change

## Verification

- `make test` green (29 new tests pass, 558+ existing unaffected)
- `make lint` (clang-tidy) clean
- `make analyze` (cppcheck) clean
- `make` builds under both `g++` and `clang++`
- Manual: `Alt+1..9` switches windows; `/window set 2` switches;
  `/window set 99` and `/window set foo` rejected with usage;
  `/window set <Tab>` lists valid numbers only; ESC still toggles scroll mode;
  Ctrl+C still cancels/quits; mouse wheel still scrolls.

## Branch

`refactor/tui-input-loop-hexagonal` off `main`. Squash-merge with scoped
imperative message:
`refactor(tui): extract God Method run() into hexagonal ports & adapters; fix Alt+number; add /window set`

## Workflow

Red (29 failing tests) → Proposal (this doc) → **Sign-off** → Green → PR.
No production code is written until this proposal is approved.
