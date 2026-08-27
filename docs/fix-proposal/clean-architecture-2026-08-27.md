# Comprehensive Clean-Up & Clean Architecture Proposal — 2026-08-27

- **Status:** Draft — awaiting sign-off
- **Branch:** `fix/clean-architecture-v1` (proposed)
- **Author:** Muse Spark (audit 2026-08-27, 4 parallel explorers + manual verification)
- **Target:** Zero technical debt (pre-alpha baseline for beta)
- **Constraints:** Pre-alpha — security deferred (`lib/workspace.cpp:61` lexical confine, `tools/bash_tool.cpp:27` dangerous list, `tools/search_tool.cpp:83` fallback parked); no `llama-turboq` service changes; `main` now green `33075234503` after `7a0e69d`
- **References:** `AGENTS.md` (Build & verify, Engineering principles, Prompting philosophy), `docs/issues.md`, `docs/fix-tracker.md`, `docs/architecture.md`, `completions.json:794`, `tests/build_hygiene.sh:126`

---

## 1. Executive Summary

The codebase is **architecturally sound** — hexagonal boundaries hold (`lib/` → `tui/` no reverse dep), DIP via `Tool`/`SearchBackend`/`LLMClient`/`AgentHooks` ports, factories own `unique_ptr`, Strategy/Registry/Observer applied correctly.  CI is green again (`check` + `lint` + `build-and-test` g++/clang++).

Remaining debt is **hygiene and size**, not direction.  The single blocker `B1` (generated `Makefile` 406 vs `Makefile.in` 394) and `P5` audit drift `2308→2295` are fixed in `7a0e69d`.  What remains is:

- a God Class (`tui/tui.h:46` 394 lines, `Tui::run` 394 lines),
- systemic `>10`-line methods (every `lib/*.cpp`, `tui/tui_input.cpp:941` 318 lines),
- one class-size violation (`lib/memory_store.cpp:115` 287 lines + `system("mkdir -p")`),
- a concurrency defect (`lib/event_bus.cpp:22` lock-in-callback),
- residual hard-rule escapes in slash dispatch (`tui/tui_input.cpp:199,245,298`),
- a masking fallback in `PluginRegistry` (`lib/plugin_registry.cpp:24`) + `Capability void*` type erasure,
- thin hygiene gaps (`.clang-tidy` narrow filter, stale `compile_flags.txt`, hardcoded mock ports `8911`, test monolith `tests/run_tests.cpp:4451`).

No speculative generality needs new code — the Capability/EventBus types are deliberately over-declared for plugin v2 phases.  The proposal pays down the 9 open items in **4 phases, 9 FIXes**, each Red→Green per `AGENTS.md`, Boy Scout for method-size, no bulk rewrite.

**Done when:** `make clean && make && make test && make lint && make analyze` zero warnings on both compilers, `make check` P5 green, `Tui` header <200, `JsonMemoryStore` <150, `EventBus::fire` deadlock-free, slash tree sole source, no new SOLID violation.

---

## 2. Context & Audit Snapshot

### 2.1 What is already excellent (keep)

| Area | Evidence | Pattern |
|------|----------|---------|
| Hexagonal | `grep -rn '#include.*tui/' lib/ include/agent/` → 0 (`lib/agent.cpp:10` only `agent/tools.h` port header) | Ports & Adapters |
| DIP/Factory | `make_*_tool()`/`make_*_backend()` → `unique_ptr` leased as `shared_ptr<Tool>` in `lib/registry.cpp:70` survives `dispatch.cpp:175` parallel `std::async` | Factory, Registry |
| Strategy | `SearchBackend` `grep` vs `semantic` via `mode` arg `tools/search_tool.cpp:107`, swap only `embed()` | Strategy |
| Observer | `AgentHooks` `std::function` lambdas + `EventBus::subscribe/intercept` `include/agent/event_bus.h:67`, `bench/recorder.cpp` | Observer, Pub/Sub |
| RAII | No raw `new/delete` except `lib/job.cpp:21` private-ctor `unique_ptr<Job>(new Job)` (documented); `HeaderList::~HeaderList` `lib/http_transport.cpp:89`, `Job::reader_` joined in `~Job()` `lib/job.cpp:52` | RAII, Rule of Zero |
| Error handling | `Tool::execute() const` never throws (`tools/*.cpp` 0 throws), `dispatch.cpp:234` try/catch → `ToolResult{false}`; library throws typed `ApiError/CancelledError` `include/agent/llm.h:60` | — |
| Context stack | `include/agent/context.h:172` pure stack `push:60/pop:72/clear:93/get_all:84` with FNV-1a `verify_chain()`, spec invariant 7 via snapshot `lib/agent.cpp:387` | Memento |
| Build hygiene | `Makefile.in:394` → `Makefile` via `configure`, `-MMD -MP` deps, `make check` P1-P4 green | — |

### 2.2 Debt inventory (verified by file:line)

| ID | Sev | File:Line | Principle / Smell | Impact | Status |
|----|-----|-----------|-------------------|--------|--------|
| **N1** | ~~Critical~~ Done | `Makefile:406` vs `Makefile.in:394`, `lib/event_bus.cpp:1`, `tui/session_browser_core.*:1` | Build determinism | Fresh checkout lost 9 objects after `./configure`; `SessionBrowser` + `EventBus`/`PluginRegistry` untracked | **Done `7a0e69d`** |
| **N2** | ~~High~~ Done | `AGENTS.md:428` / `tui/tui_input.cpp:2295` vs 2308 | Hygiene P5 | `make check` failed on every `push` | **Done `7a0e69d`** |
| **N3** | High | `tui/tui.h:46` 443 lines → `class Tui:46-439` 394; `tui/tui.cpp:750` `run:394` | SRP, God Class, `>200` | 7 reasons to change (ncurses, windows, threads, rendering, git, sessions, feeds); merge conflicts, velocity | Open |
| **N4** | High | `lib/agent.cpp:811` (`chat_once:227:116`, `ensure_system_prompt:123:78`, `run:686:71`); `tui/tui_input.cpp:941:318` (`register_builtin_actions`); `lib/compressor_parser.cpp:58:91`; `tool_call_parser.cpp:111:150`; `config.cpp:27:110`; etc — every `lib/*.cpp` | Clean Code `<10` method, minimal branching | Untestable branches, copy-paste fixes; CI does not gate size (review-only) | Open |
| **N5** | High | `lib/memory_store.cpp:115` 287 lines (`JsonMemoryStore:115-401`); `save:329:27` `std::system("mkdir -p "+dir)` | SRP, `>200` class, DRY, command injection (deferred) | Single class = scoring + persistence + evidence + migration; `system` leaks to hermetic tests | Open |
| **N6** | Medium | `lib/event_bus.cpp:22` `fire:23-37` holds `scoped_lock` while invoking | Concurrency, ISP | Handler that `subscribe/unsubscribe/fire` deadlocks; 13 event types declared `event_bus.h:67`, only `MetricsPlugin` uses it today — will be P1 when `TUIRender` fires | Open |
| **N7** | Medium | `lib/plugin_registry.cpp:24` `activate` static `s_bus/s_tools` fallback + `context:78` same; `include/agent/plugin_v2.h:46` `Capability void* impl` | DIP, OCP, YAGNI | Masks uninitialized `ctx_` (plugin sees dummy `~/.amber`), `void*` anticipates 8 capability types but only `Tool`/`Hook` wired | Open |
| **N8** | Medium | `tui/tui_input.cpp:199` `rfind("policy ")`, `245` `rfind("mcp ")`, `298` `rfind("rule")` | Hard rule `completions.json` sole source | `handle_slash:1263` tree-walk is exemplary, but these bare-namespace fallbacks duplicate `refresh_policy_feed:402` leaves (`core.config.set.policy.rule.<tool>`) and `mcp_completion_subtree` — spec `no dead legacy dispatch` | Open |
| **N9** | Medium | `lib/agent.cpp:811` file, `lib/plugin.cpp:585` (discovery+handshake+tool+install), `tui/tui_input.cpp:2295`, `lib/compressor_apply.cpp:350` | File SRP | Audit claims `agent.cpp 473→200` resolved, regrouped to 811; `plugin.cpp` 585 mixes 4 lifecycles | Open |
| **N10** | Medium | `.clang-tidy:HeaderFilterRegex 'include/agent/.*\.h'`, `compile_flags.txt:13` hardcodes `/usr/include/c++/15`, `AGENTS.md` table exempt `tui_render:716` but not `tui.h` | Hygiene | Lint hides TUI/tools warnings; editor false diagnostics; table inconsistency | Open |
| **N11** | Low | `tests/run_tests.cpp:4451` 209 `TEST`s (`tests/*.cpp` 13.6k, 658 `TEST`s), mock SSE `127.0.0.1:8911-8920` hardcoded `tests/run_tests.cpp:891x` | Test hygiene, DRY | Monolith slows CI; parallel `make -j` port collision; `shell_quote` duplicated `grep_backend.cpp:68` vs `semantic_index.cpp:77` | Open |
| **PARKED** | — | `lib/workspace.cpp:61` lexical `is_within`, `tools/search_tool.cpp:83` fallback, `tools/bash_tool.cpp:27` prefix-only `is_dangerous_shell`, `tools/write_tool.cpp:61` no `requires_approval` | Protection Proxy | Intentionally deferred pre-alpha per owner (not in scope) | Parked |

Size totals for reference (verified `wc -l`): `lib/*.cpp+*.h` 9955, `tui/*.cpp` ~7000, `tests/*.cpp` 13643, `include/agent/*.h` 4118.

---

## 3. Goals, Constraints, Principles

### 3.1 Goals

- **Zero debt claim credible:** no class >200 (except test- and method-implementation-exempt files per `tests/build_hygiene.sh`), no method >10 without extracted helper, no file-level SRP mix, no hexagonal breach.
- **SOLID preserved:** SRP (one reason to change), OCP (add `Tool`/`SearchBackend`/`Capability` via new type, not loop edit), LSP (every `Tool` substitutable), ISP (narrow `Tool` 7 methods, `SearchBackend` 1 search), DIP (depend on `Tool`/`LLMClient` ports, wiring at `lib/tools_default.cpp:30` / `tui/tui_main.cpp:106`).
- **Clean Code:** <10 lines/method, minimal branching, extracted helpers with intention-revealing names, no duplication (extract `shell_quote`), no speculative branches, no commented-out code.
- **Correct patterns only:** Strategy (`SearchBackend`), Factory (`make_*`), Registry/Service Locator (`ToolRegistry` mutex+`shared_ptr` lease), Observer (`AgentHooks` + `EventBus`), Command (`process_start/read/stop`), Protection Proxy (`Workspace::confine`), Null Object (`Agent::silent_hooks`), Memento (`run_compression` snapshot `context_.get_all()` `agent.cpp:387` → `clear+push` only on success), Adapter (`LLMClient` curl+SSE), Facade (`Agent` orchestrates), Pub/Sub (`EventBus` snapshot), Capability (`IPlugin`). No new pattern without consumer.

### 3.2 Non-Goals / Constraints

- No security hardening pre-alpha (parked N).
- No `llama-turboq:8081` service change (hard rule `AGENTS.md`).
- No prompt prohibition churn (`prompts/` descriptive, not imperative).
- No new `Capability` types until consumer exists (YAGNI).
- Preserve `Context` pure-stack invariants (`push`/`pop`/`clear`/`get_all` only, `assert(verify_chain())` `context.h:84`).

### 3.3 Style & Standards Enforced

- `clang-format -i` LLVM 4-space 100 cols `.clang-format:17`; `clang-tidy` `.clang-tidy:44` (`bugprone-*, performance-*, readability-*, clang-analyzer-*`, `HeaderFilterRegex` broadened); `cppcheck` `cppcheck_suppressions.txt:30` (`nlohmann/*`, `third_party/*` excluded, `useStlAlgorithm` etc suppressed).
- RAII `unique_ptr`/`shared_ptr`, Rule of Five/Zero, `noexcept` on pure accessors (`Tool::name() const noexcept` `tool.h:28`, `SearchBackend::name()` `search_backend.h:47`, `Config::api_url()` `config.h:165`; gap fix `job.h:61` etc <5 lines).
- Const-correctness, `const&` for non-trivial read-only params, `mutable` only for `mtx_`/gate counters (documented `config.h:114`).
- Error handling: `ToolResult{false,"",err}` never throw (`dispatch.cpp:234` try/catch → `ToolResult`); library throws `std::runtime_error` only for exceptional, `ApiError`/`CancelledError` typed for LLM.
- TDD mandatory: Red (`TEST` failing) → Green → Refactor; hermetic via `FakeLLMClient` `tests/fake_llm.h:92` and `LLMClient::parse_models`; `≥80%` new-path coverage.

---

## 4. Target Architecture

### 4.1 Layering (hexagonal — unchanged, enforced)

```
Domain core  lib/ + include/agent/   ports: Tool, SearchBackend, LLMClient, AgentHooks, Workspace, MemoryStore, Compression*, EventBus, IPlugin
Adapters     tools/ (tool adapters), tools/search/ (backends), tui/ + src/amber-cli + bench/ (hosts linking libagent_core.a + libagent_tools.a)
Wiring       lib/tools_default.cpp:30 register_default_tools; tui/tui_main.cpp:106 / src/main.cpp:290 host bootstrap; PluginRegistry at boundary
```

`lib/` never `#includes "tui/"` or `src/` (checked `grep 0`); hosts communicate only via `AgentHooks` `include/agent/agent.h:55`.

### 4.2 Command surface (JSON-driven, already DONE — keep)

`completions.json:794` single source: `{help, man, action, children}`.  `SettingRegistry::complete(ns)` returns direct children 1:1 drawer/completion; `handle_slash:1263` walks `action`; feeds `refresh_model_list` / `refresh_policy_feed:402` / `refresh_job_feed` / `mcp_completion_subtree` merge via `merge_completions_json` deep merge (`setting_registry.cpp:239`).  No hardcoded slash paths, no `complete_arg` lambdas.

### 4.3 Proposed decompositions

#### Phase A — Concurrency & Capability foundation (isolated, no UI)

- **EventBus `lib/event_bus.cpp:22`** — `fire()` snapshots `matched` interceptors+observers under `mtx_`, releases, then iterates reverse (interceptors) / forward (observers).  No lock during handler.  Fixes deadlock without changing `EventType` set.
- **PluginRegistry `lib/plugin_registry.cpp:24`** — `activate`/`context` `assert(ctx_)` or `return false` + explicit `set_context` at `Tui`/`bench` bootstrap; remove static `s_bus/s_tools/s_cfg` dummies that mask wiring bugs.  `Capability` stays `void*` for now (no new consumer) but add `// TODO Phase 5: std::variant<ToolCap,HookCap…>` comment; do not introduce `std::variant` until `Provider`/`Memory` wire-up (YAGNI).
- **MemoryStore `lib/memory_store.cpp:115`** — extract `memory_scoring.cpp` (`compute_relevance:24`, `compute_freshness:34`, `compute_score:42`), `memory_persistence.cpp` (`load:309`, `save:329` → `fs::create_directories`, `hash_content:18`, `memory_to_json/json_to_memory`), leave `JsonMemoryStore` façade <150 lines (`upsert`, `top_*`, `decay_all`, `deprecate_one:372`).  `save` `system("mkdir -p")` removed.

#### Phase B — TUI God Class → Facade

` tui/tui.h:46 ` 394-line `class Tui` owns 7 responsibilities.  Split into `Tui` **Facade** (≈80 lines: owns components, `run()` 394→ `poll_signals` + `process_input` + `idle_tick` <15 each, wiring via ctor injection) plus owned components each <200/10:

| Component | File | Extracted from | Responsibility | Pattern |
|-----------|------|----------------|----------------|---------|
| `WindowManager` | `tui/window_manager.h/.cpp` | `tui.h` `windows_/active_/new_window/switch_to/close_window/lazy_load_active` | multi-window lifecycle, `next_window_id_` | Facade helper |
| `EventRouter` | `tui/event_router.h/.cpp` | `drain_events:211-288` + `on_*` 67-74 + `agent_worker/compress_worker` | queue `event_queue_`/`mtx_`, `AgentHooks` factory `make_agent_hooks:84` | Observer adapter |
| `RenderEngine` | `tui/render_engine.h/.cpp` | `tui_render.cpp:716` `draw:53` `draw_status_bar:130` `draw_input:112` | `build_view`, `bar_segments`, `Canvas` flush | Facade |
| `SlashDispatcher` | `tui/slash_dispatcher.h/.cpp` | `register_builtin_actions:941` 318 lines, `handle_slash:75` | `ActionRegistry` + `SettingRegistry` wiring | Command |
| `FeedManager` | `tui/feed_manager.h/.cpp` | `refresh_model_list`, `refresh_policy_feed:402`, `refresh_job_feed`, `refresh_provider_feed` | feed leaf `merge_completions_json` + closure registration | Capability feed |
| `SessionController` | `tui/session_controller.h/.cpp` | `tui_session.cpp:508` `session_browser:227-418` | `SessionStore` snapshot/autosave | Facade |

All components depend on `Config&`, `ToolRegistry&`, `JobService&` etc via ctor (DIP); `Tui` owns them as `unique_ptr`.  No change to `tui/tui.cpp:78-137` ncurses lifecycle ordering (`join` before `endwin` before `save_window_sessions`).

#### Phase C — Slash & Hygiene

- Delete residual `rfind` branches `tui/tui_input.cpp:199,245,298` (bare-namespace usage page stays as `core.config.set.policy` leaf `register_action` at `tui/tui_input.cpp:1113`; `get.mcp`/`learn` via tree leaves).  `handle_slash` remains sole dispatch.
- Broaden `.clang-tidy` `HeaderFilterRegex` to `include/agent/.*\.h|tui/.*\.h` (or at least `|tui/.*` for `textutil` etc) and regenerate `compile_flags.txt` from `configure` (`-I` from `PROJECT_CPPFLAGS` + `CURL_CFLAGS`/`NCURSES_CFLAGS`) so editor diagnostics match CI `LINT_CXXFLAGS`.
- `tests/build_hygiene.sh:126` already green for P5; add SB check: `artifacts` add `session_browser_test`, `-include` check `SB_TEST_OBJ`, `plugins/metrics/*.o` clean entry (already in `Makefile.in:409`).

#### Phase D — Test & File hygiene (parallel, Boy Scout)

- `tests/run_tests.cpp:4451` — split by area into `tests/config_test.cpp`, `tests/compressor_test.cpp`, `tests/registry_test.cpp` etc; add to `UNITTEST_OBJ` `Makefile.in:138`.  Keep 658 `TEST`s, hermetic `FakeLLMClient` `tests/fake_llm.h:92` + `spawn_mock_sse` 8911→ ephemeral `bind 0 + getsockname` to avoid parallel collision (`mcp_transport_test.cpp` already does).
- `lib/agent.cpp:811` + `lib/plugin.cpp:585` file SRP — not urgent pre-alpha (header classes still <200); treat as `agent_system_prompt.cpp` / `plugin_discovery.cpp` extraction when file exceeds ~500 lines, opportunistic.
- `tools/search/grep_backend.cpp:68` `shell_quote` duplicated `semantic_index.cpp:77` → move to `include/agent/semantic_helpers.h:35`.

No new files need copyright/SPDX header (`AGENTS.md`).

---

## 5. Phased Execution (dependency-aware, Red→Proposal→Sign-off→Green→PR)

| Phase | FIX ID | Scope | Depends | Effort | Gate |
|-------|--------|-------|---------|--------|------|
| **0** | `FIX-016` | Pipeline unblock (done `7a0e69d`) — `AGENTS.md` audit + `Makefile.in` drift + untracked plugin v2 | — | S | `check`+`lint` green ✅ |
| **1** | `FIX-017` | `EventBus::fire` snapshot (deadlock) | 0 | S (2h) | `event_bus_test.cpp:130` 8 tests + `gh run` green |
| **2** | `FIX-018` | `PluginRegistry` assert + remove static fallback | 0 | S (1h) | `plugin_v2_test.cpp:197` 17 + manual `ctx_==nullptr` assert |
| **3** | `FIX-019` | `JsonMemoryStore` split + `fs::create_directories` | 0 | M (4h) | `memory_store` tests, class `JsonMemoryStore` <150, no `system()` |
| **4** | `FIX-020` | Slash residuals `rfind` removal | 2 | S (2h) | `completions_test:37`, `e2e_test:26` green, no `rfind("policy` in `tui/*.cpp` |
| **5** | `FIX-021` | `FeedManager` extraction (first Tui split) | 0 | M (6h) | `Tui` header `tui.h` 394→~280, `feed_manager.h` <100, feeds still tree-driven |
| **6** | `FIX-022` | `EventRouter` + `WindowManager` extraction | 5 | M (6h) | `drain_events` <15, `run` 394→<60 |
| **7** | `FIX-023` | `RenderEngine` + `SessionController` extraction → `Tui` <180 | 6 | M (6h) | class `Tui` <200, `make check` P5 still green |
| **8** | `FIX-024` | Hygiene: `.clang-tidy` broaden, `compile_flags.txt` generate, `build_hygiene.sh` SB entry | 0 | S (1h) | `make lint` still clean, `make check` `plugins/metrics` covered |
| **9** | `FIX-025` | Test split + ephemeral mock ports + `shell_quote` dedup | 3 | M (4h) | `run_tests.cpp` no longer monolith, `spawn_mock_sse` 0-port, `duplicates` clean |

Total ~31h engineered, 9 PRs, each `make clean && make && make test && make lint && make analyze` zero warnings on `g++/clang++` per `ci.yml:84`.

**Order rationale:** 1-4 are isolated lib fixes (no TUI coupling, no merge conflicts); 5-7 are sequential Tui splits (touch `tui.h` once per PR, avoid 394-line rebase); 8-9 parallel to 5-7.

### Branch & PR discipline

- Branch `fix/<id>-<slug>` off `main` post-`7a0e69d`, squash-merge, imperative scoped messages (`tui: extract FeedManager from Tui`).
- Draft PR early, `make check` + `make lint` per few edits (not batched).
- Reviewer checklist (mandatory per `AGENTS.md`): SOLID, hexagonal, ≤200/≤10, Red→Green, `make test` hermetic, no new `clang-tidy`/`cppcheck` findings, no dead code, `Context` pure-stack `push/pop/clear/get_all` only.

---

## 6. Detailed Designs (FIX-017..025)

### FIX-017 EventBus snapshot

```cpp
// lib/event_bus.cpp:22
bool EventBus::fire(EventType type, Event& event) {
    std::vector<InterceptorEntry> interceptors;
    std::vector<ObserverEntry> observers;
    {
        std::scoped_lock lk(mtx_);
        for (auto& e: interceptors_) if (e.type==type) interceptors.push_back(e);
        for (auto& e: observers_)     if (e.type==type) observers.push_back(e);
    }
    for (auto it = interceptors.rbegin(); it!=interceptors.rend(); ++it)
        if (!it->handler(event)) return false;
    for (auto& e: observers) e.handler(event);
    return true;
}
```
Observer/interceptor copy holds `std::function` by value; `unsubscribe` under lock remains safe.  Existing `event_bus_test.cpp:130` 8 tests cover subscribe/intercept/fire/unsubscribe/clear; add `fire_reentrancy_subscribe_inside_handler` Red test.

Pattern: Pub/Sub snapshot (defensive copy).  No API break.

### FIX-019 MemoryStore split

```
lib/memory_store.cpp          451 → façade  ~140 (JsonMemoryStore <150)
lib/memory_scoring.cpp        new  ~60  (compute_relevance/freshness/score)
lib/memory_persistence.cpp    new  ~90  (load/save/seed_from_legacy, fs::create_directories)
include/agent/memory_store_detail.h (optional) — json helpers
```

Save:

```cpp
bool JsonMemoryStore::save(const std::string& path) const {
    if (auto pos = path.find_last_of('/'); pos!=npos)
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    // ... tmp + rename
}
```

Remove `std::system`.  DRY: `upsert` memory vs skill share `hash_content` path — keep as is (YAGNI to template further).

### FIX-020 Slash residuals

Delete `tui/tui_input.cpp:199` `if (arg.rfind("policy ",0)==0)` usage page — keep `register_action("core.config.set.policy", ... cmd_set)` at `1113` as branch handler (already does `usage`); similarly `tui/tui_input.cpp:245,253` `mcp` split and `298` `rule` fallback.  After, `grep -rn 'rfind("policy\|rfind("mcp\|rfind("rule' tui/` → 0.  Tree walk supplies `get.policy.rule.<tool>` leaves.

### FIX-021 FeedManager

```cpp
// tui/feed_manager.h: <80 lines
class FeedManager {
public:
    FeedManager(SettingRegistry&, ActionRegistry&, const ToolRegistry&, JobService&, ProviderService&);
    void refresh_model_list(std::vector<ModelInfo>);
    void refresh_policy_feed(PolicyStore const&);
    void refresh_job_feed(JobService const&);
    void refresh_provider_feed();
};
```

Owns no `Tui` state; `Tui` calls `feeds_.refresh_policy_feed(*agent->policy())` after `apply_policy_rule:343`.  `register_action` idempotent already (`action_registry.cpp:24` first-wins).  Leaves-as-values preserved.

---

## 7. Verification & KPIs

Per-PR:

```
make distclean && ./configure && make -j && make test && make lint && make analyze && make check
```

Gate `ci.yml:84` matrix `g++/clang++` must green; `make check` P5 audit table refreshed (`tui_input.cpp` 2295 → after splits, update `AGENTS.md:428` / `build_hygiene.sh:110`).

Per-FIX extra:

- FIX-017: `event_bus_test` new `fire_reentrancy` RED then GREEN.
- FIX-019: `grep -c 'class JsonMemoryStore' lib/memory_store.cpp:115` definition <150 lines (`awk` count).
- FIX-021-023: `wc -l tui/tui.h` 443→<320→<200 staged; `Tui::run` 394→<60 line count via `awk '/^void Tui::run/,/^}/' tui/tui.cpp`.
- Bench `bench/runner.cpp:193` unchanged; `bash_cd_prefix` KPI untouched (prompt fix not in scope — left for `BENCH.md`).

Hermetic: `LLMClient::parse_models` for `run_tests`, `FakeClient` `bench/fake.h:51` for bench; no `:8081` live call in unit suite.

---

## 8. Best Practices Enforced by This Proposal

- **Size limits enforced by review, not compiler** — `tests/build_hygiene.sh` P5 hard-fails drift; method 10-line via human review + incremental extraction ( Boy Scout rule: leave file shorter than found).
- **No speculative branches** — if a phase's consumer doesn't exist (e.g. `Capability` `Provider` type), keep `void*` with comment; introduce `std::variant` only when `ProviderService` wiring lands.
- **DRY** — `default_excluded_dirs()`, `shell_quote` single source, `memory_scoring` single source.
- **KISS** — snapshot is 10 lines, not a lock-free queue; `fs::create_directories` not a custom `mkdir` loop.
- **Isolation** — each phase touches ≤3 files in one layer; `lib/` never depends on `tui/`.
- **Documentation** — every split updates `AGENTS.md` audit table and `docs/issues.md` Current Open row; commit message scopes (`tui:`, `fix:`, `refactor:`) imperative.

---

## 9. Risks & Mitigations

| Risk | Mitigation |
|------|------------|
| `Tui` split causes rebase pain (394-line header) | Phase 5-7 sequential, one header churn per PR; keep `tui/tui.h` splits as pure moves (no logic change) first commit, behavior second |
| `EventBus` snapshot copies `std::function` (cost) | 13 types, <10 handlers each — copy <1µs; measure via `bench` if spike |
| `JsonMemoryStore` split breaks `.amber/experience.json` migration | Keep `seed_from_legacy:418` in persistence; round-trip test `tests/run_tests.cpp` `json_to_memory` unchanged |
| `make lint` broadened to `tui/` surfaces existing warnings | Gate on *new* warnings only; fix incrementally per phase (do not batch) |
| Test split breaks `make test` parallelism (port 8911) | Switch to ephemeral port in same PR as split; run `make test -j` locally |

---

## 10. Success Criteria (beta-ready)

- `Tui` header `tui/tui.h` <200 lines, `Tui::run` <60, `register_builtin_actions` 318→<50+helpers, `class JsonMemoryStore` <150.
- `EventBus::fire` re-entrancy test green.
- Slash: `grep -rn rfind.*policy\|rfind.*mcp tui/` → 0; `completions_test` 37 + `e2e_test` 26 + `command_line_test` green.
- `make check` `all invariants hold` on fresh checkout; `make lint` + `make analyze` zero on both compilers; `gh run 33075234503` green remains baseline.
- `docs/issues.md` N3-N11 marked ✅ with `FIX-017..025` PR links; `docs/fix-tracker.md` tasks closed.

---

## 11. Appendix — File:Line Index for Reviewers

```
AGENTS.md:428                         audit table
Makefile.in:65-145,409                CORE/TUI/UNITTEST/SB + rules (done 7a0e69d)
include/agent/context.h:172           Context pure stack
include/agent/event_bus.h:67          EventBus ports
lib/event_bus.cpp:22                  fire deadlock
lib/plugin_registry.cpp:24            static fallback
include/agent/plugin_v2.h:46          Capability void*
lib/memory_store.cpp:115,329          JsonMemoryStore class + system()
tui/tui.h:46 443 lines                God Class
tui/tui.cpp:750 run:394              run monolith
tui/tui_input.cpp:199,245,298,941    hard-rule residuals + 318-line register
tui/session_browser_core.h:75        newly tracked
tests/run_tests.cpp:4451             monolith
tests/build_hygiene.sh:126           P5
.clang-tidy:HeaderFilterRegex         narrow filter
compile_flags.txt:13                  stale
```

Spec credit: `docs/spec/plugins/plugin-framework-v2.md:704`, developer guide `441`, `docs/architecture.md:258`, `AGENTS.md` Engineering principles (SOLID/KISS/DRY/YAGNI/size limits/hexagonal).

