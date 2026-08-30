# Phase 2 — TUI Facade: WindowManager + EventRouter + RenderEngine + SessionController — 2026-08-30

- **Status:** Draft — awaiting sign-off (Red → Proposal → **Sign-off** → Green → PR)
- **Branch:** `fix/022-event-router-window-manager` → `fix/023-render-session` (sequential, each squash-merged)
- **Author:** Muse Spark (audit 2026-08-30, follow-up to `clean-architecture-2026-08-27.md:321`)
- **Depends on:** `FIX-016..021` + `FIX-024/025-partial` shipped (`7a0e69d` pipeline, `4f82b15` EventBus, `54304b4` PluginRegistry, `aa8470c` MemoryStore, `7159402` slash `rfind`, `a4003f5` FeedManager, `5095698`+`48a2bc5` hygiene), `main` green `33089714293` (lint 23m59s, check/analyze/build g++/clang++ clean)
- **References:** `AGENTS.md:428` audit (`tui/tui.h:448` 448, `tui/tui_input.cpp:2174`, `tui/tui_render.cpp:716`, `tui/tui.cpp:1179`, `tui/tui_session.cpp:508`), `docs/issues.md:N3-N4`, `docs/fix-tracker.md:FIX-022..025`, `include/agent/context.h:84` pure stack, `completions.json:794` sole source, `.clang-tidy:43` (`include|tui` after `48a2bc5`), `tests/build_hygiene.sh:110` P5

---

## 1. Executive Summary

`clean-architecture-2026-08-27.md` claimed zero-debt when `Tui` header <200, `Tui::run:394` <60, `JsonMemoryStore:287` <150, `EventBus:fire` deadlock-free, slash tree sole source. **Phase 1 delivered** the last three (`EventBus` snapshot `tui/event_bus.cpp:22`, `JsonMemoryStore` → `memory_scoring.cpp:164+memory_persistence.cpp:110` 70 lines, `FeedManager:105` + `rfind` deletion). What remains is the only structural blocker:

- **God Class `tui/tui.h:448`** — `class Tui:47-444` 398 lines (limit 200) owns 7 reasons to change: ncurses lifecycle, multi-window, threads+queue, rendering, git decoration, sessions/workspace, feeds. `Tui::run:1179` 392 lines, `drain_events:217-293` 77 lines, `on_*` 8 handlers, `agent_worker:594-635`/`compress_worker:637-676`, `WindowManager` state (`windows_:407`, `active_:408`, `next_window_id_:239`, `switch_to:455`/`close_window:464`/`window_by_id:1175`), rendering (`tui_render.cpp:716`), sessions (`tui_session.cpp:508`).
- Systemic `>10`-line methods (every `lib/*.cpp` + `register_builtin_actions` now in `feed_manager` but `Tui::run` still 392) — Boy Scout per PR, not a phase.
- File SRP `lib/agent.cpp:811`/`lib/plugin.cpp:585` — tracked, opportunistic >500 (not in Phase 2).
- Test monolith `tests/run_tests.cpp:4451` — Boy Scout after facade (Phase 2 does not move tests).

Phase 2 is **pure moves + facade wiring**, no behavior change, no new pattern, no prompt/llama change, no security hardening (parked). Two sequential PRs leave `Tui` as a ≈80-line **Facade** that owns components via `unique_ptr` and forwards `run()` as `poll_signals + drain + idle_tick` (<15 each):

| PR | Component | New file | From | Lines | Gate |
|----|-----------|----------|------|-------|------|
| **022** | `WindowManager` | `tui/window_manager.h/.cpp` | `tui.h:407-408,239` `tui.cpp:171-213,455-477,1175` | <120 | `wc -l tui/window_manager.h` <80 |
| **022** | `EventRouter` | `tui/event_router.h/.cpp` | `tui.h:64-136` `tui.cpp:217-676` `event_router.h:20` existing helper | <280 | `drain_events` <15, `make_agent_hooks` owns queue |
| **023** | `RenderEngine` | `tui/render_engine.h/.cpp` | `tui/tui_render.cpp:716` `tui.h:154-214` | <320 | owns `Canvas`+`md_style` |
| **023** | `SessionController` | `tui/session_controller.h/.cpp` | `tui/tui_session.cpp:508` `tui.h:221-230` | <250 | owns `SessionStore` snapshot/autosave |

After 022: `tui.h:448` → ~300, `Tui::run:392` → ~120. After 023: `tui.h` **<200** (target 180), `Tui::run` <60, `make check` P5 still green (`AGENTS.md:428` and `build_hygiene.sh:110` updated per PR).

---

## 2. Context & What Is Already Excellent (keep)

| Area | Evidence (stay green) | Pattern |
|------|----------------------|---------|
| Hexagonal | `grep -rn '#include.*tui/' lib/ include/agent/` → 0 (`lib/agent.cpp:10` only `agent/tools.h` port) | Ports & Adapters |
| Feed leaves | `tui/feed_manager.cpp:105` `refresh_model_list/policy/provider/job` own `merge_completions_json` leaves + closures; `handle_slash:1263` tree-walks `action` | Capability feed |
| EventBus | `lib/event_bus.cpp:22` snapshots under `mtx_` then iterates (FIX-017 `4f82b15` + 2 reentrancy tests `tests/event_bus_test.cpp:130`) | Pub/Sub snapshot |
| PluginRegistry | `lib/plugin_registry.cpp:24` `assert(ctx_)`/`return false` + `tui/tui.h:399` `workspace_`/`plugin_ctx_` + `tui/tui.cpp:116` wiring (FIX-018 `54304b4`) | DIP |
| MemoryStore | `lib/memory_store.cpp:118` façade 70 + `memory_scoring.cpp:164` + `memory_persistence.cpp:110` `fs::create_directories` (FIX-019 `aa8470c`) | SRP |
| Build | `Makefile.in:65` `CORE_OBJS` includes `event_bus.o`/`plugin_registry.o`/`metrics_plugin.o`, `TUI_OBJS` includes `feed_manager.o`, `SB_TEST_OBJ` added `7a0e69d`; `-MMD -MP` deps | — |
| Hygiene | `.clang-tidy:43` `HeaderFilterRegex 'include/agent/.*\.h|tui/.*\.h'` + suppressed `modernize-concat-nested-namespaces` etc `48a2bc5`; `tui_input.cpp:2174` (`rfind` deleted `7159402`) | — |

Debt left is **one class** and **two file regroups**, not direction. No new `Capability` type, no prompt prohibition, no `llama-turboq:8081` change.

---

## 3. Goals, Constraints, Principles

**Goals:** `Tui` header <200 (`AGENTS.md` audit), `Tui::run` <60, `drain_events` <15, every new method <10 with minimal branching, no duplication, Boy Scout for `>10` sites opportunistically, `make clean && make && make test && make lint && make analyze` zero warnings both compilers, `make check` P5 green on fresh checkout.

**Non-goals:** security hardening (`workspace.cpp:61` lexical, `bash_tool.cpp:27` list — parked pre-alpha), prompt rework, `Context` pure stack (`push:60/pop:72/clear:93/get_all:84` + `assert(verify_chain())` — only `clear+push` rebuild), llama service, test monolith split (deferred).

**Principles:** SOLID (SRP — each component one reason; OCP — add window/event/render/session via new type not `Tui` edit; LSP — `WindowManager`/`EventRouter` substitutable via interface; ISP — narrow `WindowManager`/`EventRouter`/`RenderEngine`/`SessionController` ports; DIP — `Tui` ctor injects `Config&`/`ToolRegistry&`/`JobService&`/`ProviderService&`/`SessionStore&` into components as `unique_ptr`). KISS/DRY/YAGNI/size limits enforced by review (`tests/build_hygiene.sh` P5), not compiler. Hexagonal: `lib/` never `#includes "tui/"`.

---

## 4. Target Architecture

### 4.1 Layering (unchanged)

```
Domain core  lib/ + include/agent/   ports: Tool, SearchBackend, LLMClient, AgentHooks, Workspace, MemoryStore, EventBus, IPlugin
Adapters     tools/ (tools), tools/search/, tui/ + src/amber-cli + bench/  → link libagent_core.a + libagent_tools.a
Wiring       lib/tools_default.cpp:30, tui/tui_main.cpp:106, Tui ctor: constructs FeedManager/EventRouter/WindowManager/RenderEngine/SessionController
```

`lib/` has 0 `tui/` include (checked). Hosts communicate only via `AgentHooks` `include/agent/agent.h:55`.

### 4.2 Tui Facade (target `tui/tui.h` ~80 lines)

```cpp
// tui/tui.h — Facade (~80 lines: owns components, run() <60, no rendering/event/window/session logic)
class Tui {
    friend class FeedManager;
public:
    Tui(Config cfg, ToolRegistry& reg, JobService& jobs, SubAgentExecutor& subagents,
        PluginManager& plugins, PluginRegistry& plugin_reg);
    ~Tui(); // join → endwin → save_window_sessions → save_workspace
    Window& new_window(const std::string& title); // delegates to windows_
    Window& open_welcome_window();
    Window& ensure_chat_window();
    void run(); // poll_signals() + drain_router() + idle_tick() + draw()
private:
    bool poll_signals();      // consume SignalState, returns should-quit
    void idle_tick(bool had_events, CommandLine& cl);
    // forwarding: windows_->switch_to/close/window_by_id/lazy_load_active
    // rendering:  render_->draw/draw_status_bar/draw_input/draw_drawer/build_view
    // sessions:   sessions_->snapshot/autosave/save_window_sessions/load_session/session_browser
    // events:     router_->drain_events()/send_async/compress_worker/run_compression/make_agent_hooks
    Config cfg_;
    std::unique_ptr<ProviderService> providers_;
    ToolRegistry& reg_; JobService& jobs_; SubAgentExecutor& subagents_;
    PluginManager& plugins_; PluginRegistry& plugin_reg_;
    Workspace workspace_; std::unique_ptr<PluginContext> plugin_ctx_;
    std::unique_ptr<FeedManager> feeds_;
    std::unique_ptr<WindowManager> windows_;
    std::unique_ptr<EventRouter> router_;
    std::unique_ptr<RenderEngine> render_;
    std::unique_ptr<SessionController> sessions_;
    // only facade-owned state remains: ServerManager mcp_servers_, settings_path_,
    // git_project_/branch/ins/del, quit_/dirty_/scroll_mode_/drawer_* etc (or move git→RenderEngine)
};
```

`Tui` owns 5 components as `unique_ptr` (DIP, ctor injection). No component holds `Tui*` except `FeedManager` friend (existing). Components communicate via callbacks/`std::function` or `Tui`-mediated forwarding, never direct cross-include.

### 4.3 Component contracts

#### WindowManager — `tui/window_manager.h/.cpp` (<120 lines, <10/method)

Owns `std::vector<std::unique_ptr<Window>> windows_; size_t active_; size_t next_id_;`

```
Window& new_window(title, Config const&, suspending?)  // makes Agent compressor/gate/mem_store/retriever, policy init
Window& open_welcome_window()
Window& ensure_chat_window()
void switch_to(size_t idx, std::function<bool(std::string)> busy_reject, std::function<void()> draw)
void close_window(std::function<bool(std::string)> busy_reject, std::function<void()> autosave, draw)
Window& win(); const Window& win() const;
Window* by_id(size_t id);
void lazy_load_active(SessionStore&, Config&, Stats&); // restores context/meta/lines
size_t active() const; size_t count() const;
std::vector<std::unique_ptr<Window>>& all(); // for save_workspace snapshot
```

Extracted from `tui/tui.h:407-408,239` and `tui/tui.cpp:171-213,455-477,1175,419-453`. `busy_reject` is a `std::function<bool(std::string what)>` supplied by `Tui` (checks `router_->busy()`), so `WindowManager` never reads `agent_busy_` directly. No `Tui` include needed — only forward-declares `Window`, `SessionStore`, `Config`.

#### EventRouter — `tui/event_router.h/.cpp` (<280 lines)

Already exists as `tui/event_router.h:20` `route_event/deny_all/pending` helpers; Phase 2 promotes it to own the full queue/worker machinery (currently in `Tui`):

```
class EventRouter {
public:
    EventRouter(std::function<Window*(size_t)> by_id, std::function<Window&()> win);
    bool drain_events(std::vector<std::unique_ptr<Window>>& windows, size_t& active,
                      RunState& state, Stats& stats, long& ctx_used, long& live_offset,
                      std::string& running_tool, std::string& running_tool_desc,
                      std::vector<PendingToolLine>& pending_tools,
                      std::queue<AgentEvent>& pending_approvals, bool modal_open,
                      bool& shutting_down, bool& dirty);
    void send_async(std::string raw_prompt, WindowManager&, JobService const&, Config& cfg,
                    std::function<void()> git_refresh, std::function<void(std::string)> append);
    void compress_worker(Window& my_win, size_t window_id);
    AgentEvent run_compression(Window&, size_t);
    AgentHooks make_agent_hooks(size_t window_id);
    bool busy() const { return busy_.load(); }
    void request_cancel(Config& cfg) { cfg.cancel_token.request(); cancel_.store(true); }
    void shutdown(); // deny_all + shutting_down latch
private:
    std::queue<AgentEvent> queue_; std::mutex mtx_;
    std::thread thread_; std::atomic<bool> busy_{false}, cancel_{false};
    bool shutting_down_ = false;
    std::vector<PendingToolLine> pending_tools_; // moves from Tui
    // per-event handlers on_reasoning/token/tool_call/... private <10 each
};
```

Extracted from `tui/tui.h:64-136` and `tui/tui.cpp:217-676`. Internals: `drain_events` snapshots batch under `mtx_`, releases lock, then iterates (same snapshot discipline as `lib/event_bus.cpp:22`); `make_agent_hooks` returns `AgentHooks` whose lambdas capture `queue_/mtx_/cancel_` by `this`; `on_approval` promise/future stays in `EventRouter` but `resolve_approval` (ncurses `approve_dialog`) stays in `Tui` — `EventRouter::drain_events` takes a `std::function<void(AgentEvent)> resolve_approval` callback supplied by `Tui` to keep ncurses out of the router (hexagonal). `pending_tools_` + `find_pending_tool` + `advance_tool_spinners` move with it. `~EventRouter` joins.

Alternative (if review prefers): keep `EventRouter` as pure queue+hooks factory, leave `on_*` in `Tui` as callbacks — spec keeps the richer owner (queue+dispatch) because `on_*` already need `Window*` routing and `tool_display::close_tool_line`, which are TUI-only and belong with the queue.

#### RenderEngine — `tui/render_engine.h/.cpp` (<320 lines)

Owns `Canvas chat_canvas_; md::Style md_style_;` plus `git_project_/branch/ins/del` decoration (moved from `Tui` to keep prompt decoration with rendering).

```
class RenderEngine {
public:
    RenderEngine(Config const& cfg, ProviderService const* providers, JobService const& jobs,
                 ServerManager const& mcp, std::vector<std::unique_ptr<Window>> const& windows,
                 size_t const& active, RunState const& state, Stats const& stats,
                 long const& ctx_used, long const& live_offset, std::string const& running_tool,
                 std::string const& git_project, std::string const& git_branch, int ins, int del);
    void draw(Window& win, bool welcome_art, bool busy, bool working_visible,
              std::string const& running_tool_desc, std::chrono::steady_clock::time_point working_since,
              int anim_phase, bool show_reasoning);
    void draw_status_bar(std::string tail, int height, int width) const;
    void draw_input(std::string const& s, size_t cursor, std::string const& shadow, int height, int width) const;
    void draw_drawer(std::string const& input, bool drawer_open, int drawer_sel, SettingRegistry const& settings, bool modal_open) const;
    void tick_clock(Window& win, int height) const;
    void advance_tool_spinners(std::vector<PendingToolLine>&, std::function<Window*(size_t)> by_id);
    std::vector<rich::Line> build_view(Window const& w, int width, bool busy, bool working_visible, ...) const;
    std::vector<Seg> bar_segments() const; // moved from tui_render.cpp:225
    void git_refresh(std::string& project, std::string& branch, int& ins, int& del) const;
    void flush() const { doupdate(); }
private:
    // helpers: height/width/chat_top/chat_height/lines_per_page/max_scroll/utf8_len/wrap_text/timestamp/kfmt/gauge_pair <10
};
```

Extracted from `tui/tui_render.cpp:716` and `tui/tui.h:154-214,194-204`. `Tui::draw:314` 51 lines becomes `render_->draw(win(), ...) ; render_->draw_status_bar(scroll_glyph)` — `Tui` still owns the `draw` call ordering but all `Seg` assembly and canvas math lives in `RenderEngine`. `display_cols/to_wide/kfmt/gauge_pair` are free helpers in `render_engine.cpp` anonymous namespace. No `Agent` include except `bar::kfmt/pressure/gauge_bar`.

#### SessionController — `tui/session_controller.h/.cpp` (<250 lines)

Owns `SessionStore store_; std::string settings_path_;` plus `snapshot/autosave/save_window_sessions/save_session/load_session/session_browser/pending RestoredCall`.

```
class SessionController {
public:
    explicit SessionController(std::string settings_path, Config& cfg, Stats& stats, long& ctx_used);
    Session snapshot(Window& w, Config const& cfg, Stats const& stats, long ctx_used) const;
    void autosave(Window& w);
    void save_window_sessions(std::vector<std::unique_ptr<Window>>& windows, Config const& cfg, Stats const& stats, long ctx_used);
    void save_session(Window& w, Config const& cfg, Stats const& stats, long ctx_used, std::function<void(int,std::string)> append_line);
    void load_session(std::string id, WindowManager& windows, EventRouter& router, std::function<void()> draw, std::function<int(Window const&)> max_scroll);
    void session_browser(WindowManager&, EventRouter& router, std::function<void()> draw);
    void lazy_load_active(WindowManager&, std::function<int(Window const&)> max_scroll);
    void save_workspace_now(std::vector<std::unique_ptr<Window>> const& windows, size_t active);
    WorkspaceState load_workspace() { return store_.load_workspace(); }
    SessionStore& store() { return store_; }
private:
    void restore_message_lines(Message const& m, std::vector<RestoredCall>& pending, Window& w, std::function<size_t(Window&,int,std::string)> append_line);
};
```

Extracted from `tui/tui_session.cpp:508` and `tui/tui.h:221-230,304-306,404`. `Tui` constructor's `store_.load_workspace` loop (`tui.cpp:129-140`) delegates to `sessions_->load_workspace` + `windows_->new_window` per entry; `Tui::~Tui:165-168` `save_window_sessions/save_workspace_now` delegate to `sessions_`. `SessionStore` is already filesystem-isolated (`Workspace::local_dir()`), so no `Tui` ncurses leak.

---

## 5. Phased Execution (dependency-aware, Red→Proposal→Sign-off→Green→PR)

| Phase | FIX | Scope | Depends | Effort | Gate |
|-------|-----|-------|---------|--------|------|
| **2a** | `FIX-022` | `WindowManager` + `EventRouter` — header `448→~300`, `run 392→~120`, `drain_events` <15 (pure move, no logic change first commit, then `<10` helper extraction) | `FeedManager` (`a4003f5`) | M (6h) | `make lint` clean, `tui.h` line count down, `drain_events`/`on_*` each <15 |
| **2b** | `FIX-023` | `RenderEngine` + `SessionController` → `Tui` **<200** (`180` target), `run` <60, `draw`/`draw_status_bar` moved, git decoration moved | 022 | M (6h) | `wc -l tui/tui.h` <200, `awk '/^void Tui::run/,/^}/' tui/tui.cpp` <60, `make check` P5 refresh `AGENTS.md:428`/`build_hygiene.sh:110` (`tui_render:716` exempt stays, `tui.h` now measured) |

**Order rationale:** 022 touches `tui.h:64-136+239+407` and `tui.cpp:171-676` once; 023 then touches the remaining `tui_render.cpp:716`+`tui_session.cpp:508` without rebasing the 022 header churn. Both are isolated to `tui/` — no `lib/` change.

**Branch & PR discipline:** `fix/022-event-router-window-manager` off `main@48a2bc5` (`33089714293` green), draft PR early, `make lint` per few edits (not batched), second commit `<10` extractions only. Squash-merge imperative scoped (`tui: extract WindowManager and EventRouter from Tui`). 023 branches off 022 after merge (or stacked PR if reviewer prefers). No direct `main` push.

---

## 6. Detailed Designs (FIX-022..023)

### FIX-022 EventRouter + WindowManager

**Move-first, then shrink.** Commit 1: create `tui/window_manager.h/.cpp` and `tui/event_router.h/.cpp`, move verbatim, `Tui` forwards. `Makefile.in: TUI_OBJS += tui/window_manager.o tui/event_router.o`. `tui/tui.h` forward-declares both; `Tui` owns `unique_ptr`. No logic change — `make test` must stay 443 pass as proof.

Commit 2 (`<10` + `minimal branching`): split `drain_events:77` into `drain_batch()` (pop under lock → return vector), `dispatch_one(ev, w)` (switch cases → `on_*`), `route_global(ev)` (StateChange/Stats/Status). Each <15 (P5 `tui_render`/`tui_input` are exempt as method-implementation files, but `drain_events` is in `tui.cpp` and must shrink). `on_tool_call:416-437` 22 lines → `allocate_pending` + `animate_spinner` helpers. `send_async:358-396` 39 lines → `prepare_send` + `launch_worker`. `make_agent_hooks:502-592` 91 lines → one closure builder `push_for(window_id)` reused 8 times (already does — keep).

**Invariants preserved (spec § Context stack, Prompting philosophy, layering):**
- `~Tui:145-169` ordering **unchanged**: `Fputs(?1007l) → cancel_ → deny_all(event_queue+pending) under mtx → join → endwin → save_window_sessions → save_workspace`. `EventRouter::shutdown` is called under `Tui::~Tui`'s lock, not in its own dtor.
- `send_async` still `join` previous worker, swap-drain queue of stale `Done`, `busy_.store(true)` before `launch`, `agent_worker` pushes `Done` then `busy_.store(false)` — the push-before-clear ordering (`tui.cpp:634`) is load-bearing (send_async observes idle via `busy_` then drains).
- `context.h:84` `assert(verify_chain())` in `snapshot` path — `WindowManager::new_window` copies `cfg_` by value into `Agent`, no context mutation.

**Tests (Red→Green):** No new public API to unit-test pre-merge — prove by hermetic `make test` + manual: new window, `Alt+1..9` switch, `Ctrl+N` new, `close_window` last-window guard, `send_async` while busy queues `pending_prompt_`, `ESC` cancel, approvals queued while `modal_open_` and resolved after `redraw_after_modal`. Post-merge, add `tests/tui_tests.cpp` hermetic: `WindowManager::by_id` after erase, `EventRouter::drain_events` routes to correct `window_id` (use `FakeAgent` pushing `AgentEvent`).

### FIX-023 RenderEngine + SessionController

**Commit 1 (move):** `tui/render_engine.h/.cpp` receives `tui_render.cpp:716` verbatim (`build_view`, `build_view_without_working`, `max_scroll`, `bar_segments:225-312` 88 lines, `draw:314-365`, `draw_status_bar:367-495`, `tick_clock:497-510`, `advance_tool_spinners:512-536`, `draw_input:538-648`, `draw_drawer:660-714`, `wrap_text/utf8_len/timestamp/kfmt/gauge_pair` statics). `tui/session_controller.h/.cpp` receives `tui_session.cpp:508` (`snapshot:65-86`, `autosave:88-100`, `save_session:102-116`, `load_session:118-167`, `session_browser:227-417`, `lazy_load_active:419-453`, `switch_to:455-462`, `close_window:464-477`, `save_workspace_now:481-492` — note `switch_to`/`close_window` already moved in 022; SessionController takes `snapshot/autosave/load/save_workspace` only).

`Tui` keeps only `draw()` call ordering and `git_refresh` delegation (or moves `git_*` fields into `RenderEngine` — preferred: `RenderEngine` owns `git_project_/branch/ins/del` so `bar_segments` needs no `Tui` capture).

**Commit 2 (shrink):** `bar_segments:88` → `mode_seg()` + `latency_seg()` + `tps_seg()` + `jobs_seg()` + `mcp_seg()` each <10, `draw_status_bar:129` → extract `budget` + `put` already done, just keep budget calc <15, `session_browser:191` (already `build_hygiene` exempt as implementation file) stays as is — no shrink required by policy (method-implementation files exempt). `Tui::run:392` →

```cpp
void Tui::run() {
    git_refresh(); draw(); draw_input("");
    build_settings(); (void)commands(); refresh_completions();
    feeds_->refresh_all(); // model+policy+job+provider
    CommandLine cl; cl.set_history(windows_->win().prompt_history);
    auto update_completions = [&]{ /* existing */ };
    update_completions();
    while (!quit_) {
        if (poll_signals()) break;
        bool had = router_->drain_events(...);
        jobs_.check_timeouts();
        handle_fill(cl);
        int ch = getch();
        if (ch==ERR) { idle_tick(had, cl); continue; }
        if (handle_window_keys(ch, cl)) continue;
        route_commandline(ch, cl);
    }
}
```

`poll_signals:30` (consume `SignalState`, `deny_all`, `endwin`, `join+save` when `!busy`), `idle_tick:25` (had_events?draw:tick_clock, spinner, dirty flush, pending_prompt), `route_commandline:150` stays in `Tui` (CommandLine + slash dispatch are not rendering/session). `run` thus <60.

**P5 audit update:** `tests/build_hygiene.sh:110` currently checks `tui/tui_input.cpp:2174` and exempts `tui_render.cpp:716`; after 023, `tui/tui.h:448→~170` passes, `tui_render.cpp` is gone (now `render_engine.cpp:716` exempt likewise), `tui_input.cpp:2174` unchanged — update `AGENTS.md:428` table `tui/tui.h 448→~170` + add `render_engine.cpp`/`window_manager.cpp`/`event_router.cpp` to exempt list if they exceed 200 as implementation files.

---

## 7. Verification & KPIs

Per-PR, both compilers:

```
make distclean && ./configure && make -j && make test && make lint && make analyze && make check
```

Gate `ci.yml:84` `g++/clang++` must green (baseline `33089714293` 23m59s lint). `make lint` still `include|tui` `HeaderFilterRegex` — run per few edits, not batched.

Per-FIX extra:

- FIX-022: `grep -rn 'rfind.*policy\|rfind.*mcp' tui/` → 0 (already `7159402`); `wc -l tui/tui.h` `448→~300`; `awk '/^bool Tui::drain_events/,/^}/ {n++} END{print n}' tui/event_router.cpp` <15; `git log --oneline` shows move-only then shrink commits.
- FIX-023: `wc -l tui/tui.h` <200 (target 180); `awk '/^void Tui::run/,/^}/' tui/tui.cpp` <60; `make check` `all invariants hold`; manual: welcome mural, chat resize, scroll P:%, tool spinner animation (round/square), session `load` preserves `ctx_used`/`ctx_size`/`latency_ms` (round-trip `snapshot` meta), `session_browser` search `/` + delete `Ctrl+D`.
- Hermetic: `LLMClient::parse_models` still tested via `FakeLLMClient` `tests/fake_llm.h:92`; `FakeMcp` for `mcp_servers_.snapshot()`.

No bench KPI change (`bash_cd_prefix` stable); `bench/runner.cpp:193` untouched.

---

## 8. Best Practices Enforced by This Proposal

- **Size by review:** `tests/build_hygiene.sh` P5 hard-fails `tui.h>200` (not compiler); method 10-line via human review + incremental extraction (Boy Scout: leave file shorter than found).
- **No speculative branches:** no new `Capability` type, no `ConfigSource` abstraction, no `Workspace` instance refactor — YAGNI until consumer.
- **DRY:** `shell_quote` already deduped to `semantic_helpers.h:35` (`5095698`), `default_excluded_dirs()` single source, `memory_scoring` single source — no new duplication introduced; `bar_segments` helpers stay DRY with `kfmt/gauge_pair` free functions.
- **KISS:** snapshot discipline (EventBus `lib/event_bus.cpp:22` pattern reused for `EventRouter` queue), `fs::create_directories` not `system`, push-before-clear `busy_` not a lock-free queue.
- **Isolation:** each PR touches ≤4 files in `tui/` only; `lib/` never depends on `tui/`.
- **Docs:** each split updates `AGENTS.md:428` audit + `docs/issues.md:N3` line + `docs/fix-tracker.md:FIX-022/023` tasks; commit scopes `tui: extract ...` imperative.

---

## 9. Risks & Mitigations

| Risk | Mitigation |
|------|------------|
| `tui.h` split rebases pain (448-line header) | 022→023 sequential, one header churn per PR; Commit 1 pure move (no logic) diff is `git mv` + forward, reviewer can `diff --stat` |
| `~Tui` teardown order regression (join→endwin→save) | Keep ordering verbatim in `Tui::~Tui`; `EventRouter` never `join` in its own dtor — `Tui` drives it. Add `assert(!busy_.load() || shutting_down_)` in `WindowManager::close_window` guard |
| `EventRouter` copies `std::function` (cost) | 10 event types, <5 handlers each — copy <1µs; snapshot is 5 lines |
| `RenderEngine` needs `Window` lines for `max_scroll` (canvas coupling) | Pass `Window const&` + `width` only; no `Tui` capture. `Canvas` stays owned by `RenderEngine`, not `Window` |
| `SessionController` round-trip `snapshot` meta drift | Keep `ctx_used_/ctx_size/stats_` refs in `Tui` (not moved) until 023 proves meta path; 023 moves them only after `load_session` hermetic test green |
| `make lint` broadened to `tui/` surfaces warnings | Suppressions `48a2bc5` already handle `modernize-concat-nested-namespaces` etc; fix incrementally per phase (do not batch) |

---

## 10. Success Criteria (beta-ready after 023)

- `tui/tui.h:448` **<200** (target 180), `Tui::run:392` **<60**, `drain_events:77` <15, `bar_segments:88` decomposed to <10 helpers, no class >200 (except `run_tests.cpp` monolith — deferred), no method >10 without helper (implementation files `tui_input/render/session` remain exempt).
- Slash: `grep -rn 'rfind.*policy\|rfind.*mcp' tui/` → 0 (already `7159402`); `completions_test:37` + `e2e_test:26` green.
- `make check` `all invariants hold` on fresh `make distclean && ./configure` (P5 `tui_input 2174` unchanged, `tui.h` now <200).
- `make lint` + `make analyze` zero on both compilers; `gh run` green remains baseline `33089714293`.
- `docs/issues.md:N3` ✅ `FIX-022/023` PR links; `docs/fix-tracker.md` tasks closed.

---

## 11. Appendix — File:Line Index for Reviewers

```
AGENTS.md:428                          audit table (tui.h 448→180, tui_input 2174)
Makefile.in:65-145,409                 CORE/TUI/UNITTEST/SB + rules (add window_manager.o/event_router.o/render_engine.o/session_controller.o)
include/agent/context.h:84,60,72,93    Context pure stack + FNV-1a
include/agent/event_bus.h:67           EventBus ports (snapshot precedent)
lib/event_bus.cpp:22                   fire snapshot (FIX-017)
lib/memory_store.cpp:118               JsonMemoryStore <150 (FIX-019)
tui/tui.h:47 448 lines                 God Class (target <200)
tui/tui.cpp:750 run:392               run monolith (target <60)
tui/tui.h:64-136 drain/on_*           event machinery (target <15 each)
tui/tui.h:407-408 next_window/active   window state
tui/tui.cpp:129-140 workspace restore  WindowManager owns
tui/tui.cpp:145-169 ~Tui teardown      load-bearing ordering
tui/tui.cpp:171-213 new_window         WindowManager owns
tui/tui.cpp:217-293 drain_events      EventRouter owns
tui/tui.cpp:358-676 send_async/workers EventRouter owns
tui/tui_render.cpp:716 716 lines       RenderEngine owns
tui/tui_render.cpp:225 bar_segments88  RenderEngine helpers
tui/tui_session.cpp:508 508 lines      SessionController owns
tui/feed_manager.h:21 105 lines        already extracted (FIX-021 a4003f5)
tui/event_router.h:20                  existing helper → expanded
tests/build_hygiene.sh:110             P5 (update tui.h + add render_engine exempt)
.clang-tidy:43 HeaderFilterRegex       include|tui (48a2bc5)
completions.json:794                   sole source (no change)
```

Spec credit: `docs/spec/plugins/plugin-framework-v2.md:704`, `docs/architecture.md:258`, `AGENTS.md` Engineering principles (SOLID/KISS/DRY/YAGNI/size limits/hexagonal), `tui/tui.cpp:145-676` teardown+lifecycle invariants, `lib/event_bus.cpp:22` snapshot pattern, `bench/runner.cpp:193` hermetic boundary.
