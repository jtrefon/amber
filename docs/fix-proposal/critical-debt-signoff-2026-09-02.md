# Critical Technical Debt — Sign-Off Proposal — 2026-09-02

- **Status:** Draft — awaiting sign-off
- **Branch:** `fix/critical-debt-v1` (proposed)
- **Depends on:** `main` at `c93b86f` (post plugin v2 merge). Must not conflict with in-flight plugin session.
- **Author:** Code review audit 2026-09-02
- **References:** `docs/fix-proposal/clean-architecture-2026-08-27.md` (Phase 1), `docs/fix-proposal/phase2-tui-facade-2026-08-30.md` (Phase 2), `docs/issues.md`, `AGENTS.md`

---

## 1. Executive Summary

Code review identified 18 findings. This proposal covers the **critical-severity items that are correctness bugs** (not style or size), plus the **build-blocking portability issue**. These are independent of the in-flight plugin work and safe to merge alongside it.

| # | Severity | Issue | Fix class | TDD? |
|---|----------|-------|-----------|------|
| 1 | 🔴 Critical | `EventBus::fire` re-entrancy deadlock | Snapshot under lock, release, then invoke | Yes — Red test first |
| 2 | 🔴 Critical | `PluginRegistry::context()` static fallback masks null | `assert(ctx_)` + remove fallback | Yes — Red test first |
| 3 | 🔴 High | `<sys/sysinfo.h>` not portable (macOS build broken) | `#ifdef` platform guard + macOS `sysctl` | Yes — build on macOS |
| 4 | 🟡 Medium | `PolicyStore::next_id_` unused field | Remove field | No (trivial) |
| 5 | 🔵 Low | Missing `noexcept` on ~30 accessors | Bulk annotate | No (mechanical) |

**Non-goals (deferred to avoid conflict with plugin session):**
- `Tui::run()` god method — tracked in `docs/fix-proposal/phase2-tui-facade-2026-08-30.md`
- `SlashDispatcher::register_builtin_actions()` monolith — tracked in Phase 2
- `Agent::chat_once()` / `Config::load()` / `CompressionPipeline::compress()` method size — Boy Scout per PR
- `SlashDispatcher` god class — Phase 2
- EventBus 13 unused event types — YAGNI; keep declared for plugin v2
- Memory scoring magic numbers / duplication — tracked separately

---

## 2. Detailed Designs

### FIX-D1 — EventBus::fire re-entrancy deadlock

**Problem:** `lib/event_bus.cpp:22` `fire()` holds `std::scoped_lock lk(mtx_)` while invoking callbacks. A callback that calls `subscribe()`/`unsubscribe()`/`fire()` re-enters the same mutex and deadlocks.

**Fix:** Snapshot the matched subscriber list under the lock, release the lock, then invoke. Pattern already partially used (the copy loop exists) — just need to release before iterating.

```cpp
// lib/event_bus.cpp — FIX-D1
bool EventBus::fire(EventType type, Event& event) {
    std::vector<InterceptorEntry> interceptors;
    std::vector<ObserverEntry> observers;
    {
        std::scoped_lock lk(mtx_);
        for (auto& e : interceptors_)
            if (e.type == type) interceptors.push_back(e);
        for (auto& e : observers_)
            if (e.type == type) observers.push_back(e);
    }
    for (auto it = interceptors.rbegin(); it != interceptors.rend(); ++it)
        if (!it->handler(event)) return false;
    for (auto& e : observers)
        e.handler(event);
    return true;
}
```

**Test (Red → Green):**

```cpp
// tests/event_bus_test.cpp — new test
TEST(event_bus_fire_reentrancy) {
    EventBus bus;
    Event ev{EventType::AgentTurnStart, nullptr, false};
    size_t nested_id = 0;
    size_t outer_id = bus.subscribe(EventType::AgentTurnStart,
        [&](const Event&) {
            // Subscribe inside the handler — deadlocks before fix
            nested_id = bus.subscribe(EventType::AgentTurnEnd,
                [](const Event&){});
        });
    ASSERT(bus.fire(EventType::AgentTurnStart, ev));
    ASSERT_NE(nested_id, 0u); // nested subscription succeeded
    bus.unsubscribe(outer_id);
    bus.unsubscribe(nested_id);
}

TEST(event_bus_fire_reentrancy_interceptor) {
    EventBus bus;
    Event ev{EventType::ToolCallBefore, nullptr, false};
    size_t nested_id = 0;
    size_t outer_id = bus.intercept(EventType::ToolCallBefore,
        [&](Event& e) {
            nested_id = bus.subscribe(EventType::ToolCallAfter,
                [](const Event&){});
            return true;
        });
    ASSERT(bus.fire(EventType::ToolCallBefore, ev));
    ASSERT_NE(nested_id, 0u);
    bus.unsubscribe(outer_id);
    bus.unsubscribe(nested_id);
}
```

**Verification:** `make test` — both new tests pass. Existing 8 `event_bus_test` tests unchanged. `make lint && make analyze` zero new warnings.

**Files touched:** `lib/event_bus.cpp` only (production). `tests/event_bus_test.cpp` (test).

---

### FIX-D2 — PluginRegistry static fallback masks null context

**Problem:** `lib/plugin_registry.cpp:71-82` `context()` returns a `static` fallback with default-constructed `EventBus`/`ToolRegistry`/`Config`/`Workspace` when `ctx_` is null. A plugin activated before `set_context()` silently gets a dead EventBus — events are lost, tools are empty, config is defaults, workspace is `~/.amber`.

**Fix:** Replace the static fallback with `assert(ctx_)` in debug builds and a clear error return. Also harden `activate()` to return `false` when `ctx_` is null.

```cpp
// lib/plugin_registry.cpp — FIX-D2
const PluginContext& PluginRegistry::context() const {
    assert(ctx_ && "PluginRegistry::context() called before set_context()");
    return *ctx_;
}

bool PluginRegistry::activate(const std::string& id) {
    auto it = entries_.find(id);
    if (it == entries_.end()) return false;
    if (it->second.state == State::Active) return true;
    if (!ctx_) {
        assert(ctx_ && "PluginRegistry::activate() called before set_context()");
        return false;
    }
    bool ok = it->second.plugin->initialize(*ctx_);
    it->second.state = ok ? State::Active : State::Failed;
    return ok;
}
```

**Test (Red → Green):**

```cpp
// tests/plugin_v2_test.cpp — new test
TEST(plugin_registry_context_assert) {
    // Removing static fallback means calling context() before
    // set_context() asserts. Test that activate returns false.
    PluginRegistry reg;
    auto plugin = std::make_shared<TestPlugin>("test-id", "1.0", "test");
    reg.register_plugin(plugin);
    // No set_context() called — activate must fail, not silently succeed
    ASSERT(!reg.activate("test-id"));
    ASSERT(reg.state("test-id") == PluginRegistry::State::Discovered);
}

TEST(plugin_registry_context_after_set_context) {
    PluginRegistry reg;
    EventBus bus;
    ToolRegistry tools;
    Config cfg;
    Workspace ws;
    PluginContext ctx{bus, tools, cfg, ws};
    reg.set_context(&ctx);

    auto plugin = std::make_shared<TestPlugin>("test-id", "1.0", "test");
    reg.register_plugin("test-id", plugin);
    ASSERT(reg.activate("test-id"));
    ASSERT(reg.state("test-id") == PluginRegistry::State::Active);
    reg.shutdown_all();
}
```

**Verification:** `make test` — both new tests pass. Existing `plugin_v2_test.cpp` 17 tests unchanged. `make lint && make analyze` zero new warnings.

**Files touched:** `lib/plugin_registry.cpp` (production). `include/agent/plugin_registry.h` (no change — interface unchanged). `tests/plugin_v2_test.cpp` (test).

---

### FIX-D3 — `<sys/sysinfo.h>` not portable (macOS build broken)

**Problem:** `lib/environment.cpp:14` `#include <sys/sysinfo.h>` is Linux-specific. The build fails on macOS with `fatal error: 'sys/sysinfo.h' file not found`. The codebase claims to be portable C++17.

**Fix:** Guard the Linux-specific include and implement macOS equivalents using `sysctl()` and `host_statistics()`.

```cpp
// lib/environment.cpp — FIX-D3
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/sysinfo.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <mach/mach.h>
#endif
```

For the specific usage sites (need to read the full file to verify each call):

```cpp
// Uptime — Linux: sysinfo, macOS: sysctl CTL_KERN_KERN_BOOTTIME
// Memory — Linux: sysinfo, macOS: host_statistics
// Load avg — both: getloadavg()
```

Implement a `SystemInfo` struct with platform-specific fill:

```cpp
namespace {
struct SystemInfo {
    long uptime_secs = 0;
    unsigned long total_ram = 0;
    unsigned long free_ram = 0;
};

#if defined(__linux__)
SystemInfo read_system_info() {
    SystemInfo info;
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        info.uptime_secs = si.uptime;
        info.total_ram = si.totalram * si.mem_unit;
        info.free_ram = si.freeram * si.mem_unit;
    }
    return info;
}
#elif defined(__APPLE__)
SystemInfo read_system_info() {
    SystemInfo info;
    // Boot time → uptime
    struct timeval boottime;
    size_t len = sizeof(boottime);
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    if (sysctl(mib, 2, &boottime, &len, nullptr, 0) == 0) {
        info.uptime_secs = time(nullptr) - boottime.tv_sec;
    }
    // Host statistics → memory
    mach_msg_type_number_t count = HOST_VM_INFO_COUNT;
    vm_statistics_data_t vmstat;
    if (host_statistics(mach_host_self(), HOST_VM_INFO,
                        (host_info_t)&vmstat, &count) == KERN_SUCCESS) {
        info.total_ram = vmstat.wire_count + vmstat.active_count +
                         vmstat.inactive_count + vmstat.free_count;
        info.total_ram *= vm_page_size;
        info.free_ram = vmstat.free_count * vm_page_size;
    }
    return info;
}
#endif
} // namespace
```

**Test:** Manual — `make lib` compiles on macOS (primary verification). No new unit test needed (the function is an integration probe, not pure logic). Existing `environment_test` in `run_tests.cpp` covers the formatting path.

**Files touched:** `lib/environment.cpp` only.

---

### FIX-D4 — Remove unused `PolicyStore::next_id_`

**Problem:** `include/agent/policy.h:83` declares `int next_id_ = 0;` but it is never read or written. Produces `-Wunused-private-field` during build.

**Fix:** Remove the field.

```cpp
// include/agent/policy.h — FIX-D4
private:
    std::vector<PolicyRule> rules_;
    std::set<std::string> session_grants_;
    std::unordered_map<std::string, PolicyLevel> last_choices_;
    // REMOVED: int next_id_ = 0;  (unused, was producing -Wunused-private-field)

    PolicyRule* mutable_find(const std::string& tool);
```

**Test:** No new test needed. Build must produce zero `-Wunused-private-field` warnings for this class.

**Files touched:** `include/agent/policy.h` only.

---

### FIX-D5 — Bulk noexcept annotation on trivial accessors

**Problem:** ~30 accessors across the codebase that never throw are not marked `noexcept`. This prevents compiler optimizations and generates unnecessary exception-handling code. Targets identified in review:

| File | Methods |
|------|---------|
| `include/agent/agent.h` | `context()`, `set_hooks()`, `set_detection_loop()`, `set_detection_duplicate()`, `silent_hooks()` |
| `include/agent/event_bus.h` | `clear()`, `unsubscribe()` |
| `include/agent/registry.h` | `empty()`, `snapshot_tools()`, `find()` |
| `include/agent/config.h` | `validate()`, `save_global()`, `save_settings()` |
| `include/agent/compressor.h` | `DefaultCompressionGate::last_decision()`, `should_compress()` |
| `include/agent/experience.h` | `MemoryStore::store_size()`, `deprecate()`, `set_promoted()` |
| `include/agent/plugin.h` | `PluginManager::plugins()`, `find()`, `get_setting()` |
| `include/agent/plugin_v2.h` | `IPlugin::id()`, `name()`, `version()`, `capabilities()` |
| `tools/*.h` (tool headers) | `Tool::name()`, `is_read_only()`, `requires_approval()` |
| `include/agent/search_backend.h` | `SearchBackend::name()` |

**Fix:** Annotate each with `noexcept`. Most are one-line changes in header declarations.

**Test:** No new tests needed. Existing tests must still pass. `make lint` must not regress.

**Files touched:** ~10 header files, one-line changes each.

---

## 3. Execution Plan

| FIX | Scope | Dependencies | Effort | Gate |
|-----|-------|-------------|--------|------|
| D1 | `lib/event_bus.cpp` + `tests/event_bus_test.cpp` | None | 1h | `make test` reentrancy tests green |
| D2 | `lib/plugin_registry.cpp` + `tests/plugin_v2_test.cpp` | None | 1h | `make test` context-assert tests green |
| D3 | `lib/environment.cpp` | None | 1h | `make lib` compiles on macOS |
| D4 | `include/agent/policy.h` | None | 5min | No `-Wunused-private-field` |
| D5 | ~10 header files | None | 30min | `make lint` zero warnings |

**Total:** ~4h engineered, single PR, each commit independently verifiable.

**Order:** D4 (trivial), D5 (mechanical), D3 (isolated), D1+D2 (tested together — both touch event system).

---

## 4. Red → Green Test Sequence (TDD)

### D1 — EventBus re-entrancy

1. **RED:** Write `TEST(event_bus_fire_reentrancy)` and `TEST(event_bus_fire_reentrancy_interceptor)` — both deadlock/hang before fix.
2. **GREEN:** Apply the snapshot fix in `lib/event_bus.cpp`.
3. **REFACTOR:** Verify existing 8 EventBus tests still pass, `make lint` clean.

### D2 — PluginRegistry context

1. **RED:** Write `TEST(plugin_registry_context_assert)` — passes if static fallback removed (assert or return false).
2. **GREEN:** Apply the `assert(ctx_)` + `activate` guard in `lib/plugin_registry.cpp`.
3. **REFACTOR:** Add `TEST(plugin_registry_context_after_set_context)` to verify happy path. Verify existing 17 plugin_v2 tests still pass.

---

## 5. Verification

```bash
make distclean && ./configure
make -j && make test && make lint && make analyze && make check
```

- **CI:** `g++` and `clang++` both green.
- **macOS:** `make lib` must compile (FIX-D3).
- **Hermetic:** No live LLM calls. All tests use `FakeLLMClient` (`tests/fake_llm.h`).
- **No prompt/llama change:** `prompts/`, `completions.json`, `llama-turboq:8081` untouched.

---

## 6. Conflict Avoidance with Plugin Session

All 5 fixes are confined to:
- `lib/event_bus.cpp` + `include/agent/event_bus.h` (D1)
- `lib/plugin_registry.cpp` + `tests/plugin_v2_test.cpp` (D2)
- `lib/environment.cpp` (D3)
- `include/agent/policy.h` (D4)
- ~10 header files in `include/agent/` + `tools/` + `include/agent/` (D5)

If the plugin session touches `plugin_registry.cpp` or `plugin_v2_test.cpp`, we coordinate merge order: this PR merges first (its changes are additive — assert guard + tests), then the plugin session rebases. No structural conflicts expected.

---

## 7. Sign-Off Checklist

- [ ] Architecture proposal accepted before production code (step 3)
- [ ] Red tests written and committed first (step 1)
- [ ] Production code makes tests green (step 4)
- [ ] `make clean && make && make test && make lint && make analyze` all pass (step 5)
- [ ] Zero new clang-tidy / cppcheck warnings
- [ ] No hexagonal boundary violations
- [ ] No dead code / speculative branches
- [ ] Commit messages: imperative, scoped (`fix: EventBus re-entrancy deadlock in fire()`)
