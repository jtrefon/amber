# Comprehensive Fix Proposal — amber (cpp-agent)

- **Based on:** Audit report dated 2026-07-24
- **Target:** Zero technical debt, industry-standard practices, production readiness
- **Workflow:** Every fix follows **Red → Proposal → Sign-off → Green → PR** (AGENTS.md §Dev workflow)
- **Verification gate:** `make clean && make && make test && make lint && make analyze` — must pass with zero warnings

---

## How to read this document

| Prefix | Source | Meaning |
|--------|--------|---------|
| `FIX-NNN` | Existing `docs/fix-tracker.md` | Previously identified, carried forward |
| `NEW-NNN` | This proposal | Newly identified in the audit |
| `[carry]` | — | Already in fix-tracker; included here for completeness |
| `[new]` | — | First appearance in this proposal |

**Status:** `[pending]` — not started. Each task is the **Proposal** step (step 2 of Red→Proposal→Sign-off→Green→PR). After sign-off, the implementer writes the Red test (step 1) and Green fix (step 4).

---

## Dependency graph

```
Phase 1 — Safety & correctness (blockers)
  NEW-001  Signal handler UB
  NEW-002  SessionStore const correctness + new_id() data race
  NEW-003  RAII-wrap CURL handles
  FIX-001  Move cancel token into core [carry from fix-tracker]
  FIX-002  Fix detached thread [carry]

Phase 2 — Architecture & duplication
  NEW-004  Factor HTTP transport (dedup post/stream_completion)
  NEW-005  Fix SessionStore::rebuild_index recursion hazard
  FIX-003  Decompose Agent::run() [carry]
  FIX-004  Decompose Agent::compress_now() [carry]
  NEW-006  Extract CompressionReporter from inline class

Phase 3 — Extensibility & coupling
  NEW-007  Abstract LLMClient behind interface
  NEW-008  Split Config god struct into domain-specific configs
  FIX-010  Build system split (core vs tools archives) [carry]

Phase 4 — Tool & security
  NEW-009  BashTool: implement read-only auto-approval
  NEW-010  dispatch_tool_calls: parallel futures with out-of-order collection
  FIX-011  Remove naive memory heuristic [carry]

Phase 5 — Testing & CI
  NEW-011  Migrate to Catch2 test framework
  FIX-005  Decouple tests from TUI headers [carry]
  NEW-012  Add integration tests for Agent::run()
  NEW-013  Add unit tests for compression pipeline, dispatch, tool recovery
  FIX-012  noexcept correctness [carry]
  FIX-013  opendir → filesystem [carry]
  FIX-014  Workspace test isolation [carry]

Phase 6 — Build & tooling
  NEW-014  Replace hand-rolled configure with CMake
  NEW-015  Un-suppress clang-tidy checks
  NEW-016  Sync compile_flags.txt with real build
  NEW-017  Move vendored deps to submodules / FetchContent
  FIX-006  Lighten dispatch.h includes [carry]

Phase 7 — Documentation (any time, parallel)
  FIX-007  Document design patterns [carry]
  FIX-008  Add TDD policy [carry]
  FIX-009  Add code review + error handling conventions [carry]
  NEW-018  Remove umbrella include/agent.h, enforce IWYU
```

---

## Phase 1 — Safety & correctness

---

### NEW-001 — Fix signal handler undefined behaviour 🔴 CRITICAL

| Field | Value |
|-------|-------|
| **Files** | `tui/tui.cpp`, `tui/tui.h` |
| **Severity** | 🔴 Critical |
| **Estimated effort** | 2 hours |

#### Problem

`tui/tui.cpp:23-30` installs a signal handler for `SIGHUP`/`SIGTERM` that calls `save_workspace_now()` and `_Exit(1)`. `save_workspace_now()` almost certainly acquires mutexes and performs I/O. Calling non-async-signal-safe functions from a signal handler is **undefined behavior** per POSIX. The OS can deliver the signal while the program is holding a mutex, causing immediate deadlock or heap corruption.

```cpp
static void signal_handler(int sig) {
    (void)sig;
    if (signal_tui_instance)
        signal_tui_instance->save_workspace_now();  // UB
    _Exit(1);
}
```

#### Target architecture

**Option A (Preferred): Self-pipe trick + deferred save**

1. Replace `signal_handler` with a bare-minimum write to a `std::atomic<bool>` or a self-pipe:
   ```cpp
   static std::atomic<bool> graceful_shutdown_requested{false};
   static void signal_handler(int) {
       graceful_shutdown_requested.store(true);
   }
   ```
2. In `Tui::run()`, check `graceful_shutdown_requested` on every tick (after `getch()` timeout).
3. When set, call `save_workspace_now()` from the main thread (safe context), then `break` out of the loop.
4. `Tui::~Tui()` already handles the join + save; just let the destructor run normally.

**Option B (Simpler): `sigaction` with `SA_RESTART` + flag**

Same as A but using `sigaction` and `sig_atomic_t` instead of `std::atomic<bool>`. The key constraint is the flag is set, not acted upon, inside the handler.

#### Refactor rules

- The signal handler must be **async-signal-safe**. It MAY write to a `volatile std::sig_atomic_t` or `std::atomic<bool>` (which is lock-free on all supported platforms). It MUST NOT call any library function (I/O, allocation, mutex).
- Remove the `::fprintf` and `::fflush` calls from the handler.
- Keep the existing logic (save workspace, clean exit) but move it to the main loop.
- Document that the self-pipe trick is the canonical pattern if async-safe I/O is ever needed.

#### Verification

- [ ] `make clean && make && make test` passes
- [ ] `SIGTERM` to a running TUI instance saves the workspace and exits cleanly within 2 seconds (test with `kill -TERM <pid>`)
- [ ] `SIGHUP` on a terminal close does the same
- [ ] No non-async-signal-safe functions remain inside signal handlers (`grep 'signal_handler' -A10 tui/tui.cpp` shows only flag write + return)

---

### NEW-002 — Fix SessionStore const correctness and `new_id()` data race 🔴 CRITICAL

| Field | Value |
|-------|-------|
| **Files** | `lib/session.cpp`, `include/agent/session.h` |
| **Severity** | 🔴 Critical (data race) + 🟠 High (const violation) |
| **Estimated effort** | 3 hours |

#### Problem (A): `const` methods modify member state

`SessionStore::save()`, `SessionStore::remove()` (and potentially others) are declared `const` but mutate `cache_valid_` and `list_cache_`. This is a lie to callers and can cause undefined behavior if the compiler optimizes based on the `const` promise.

```cpp
// lib/session.cpp
bool SessionStore::save(Session& s) const {
    // ...
    cache_valid_ = false;   // mutation through const!
    return static_cast<bool>(f);
}
```

#### Problem (B): Data race in `new_id()`

```cpp
std::string SessionStore::new_id() {
    static long long counter = 0;
    return std::to_string(now_ms()) + "-" + std::to_string(counter++);
}
```

`counter++` is not atomic. If called concurrently from multiple threads, this is a **data race** (UB). The class's public API does not document thread-safety guarantees, and the `cache_valid_` flag suggests multi-threaded access was anticipated.

#### Target architecture

**For (A):** Use `mutable` for cache-related members, or remove `const` from the API.

**For (B):** Use `std::atomic<long long>` for the counter, or switch to a UUID-based ID generator.

#### Refactor rules

1. Change `cache_valid_` and `list_cache_` to `mutable`:
   ```cpp
   mutable bool cache_valid_ = false;
   mutable std::vector<SessionMeta> list_cache_;
   ```
2. Remove `const` from `save()`, `remove()` — they are semantically mutating.
3. OR: if the `const` API is intentional (query-like), keep `const` + `mutable`.
4. Fix the data race:
   ```cpp
   std::string SessionStore::new_id() {
       static std::atomic<long long> counter{0};
       return std::to_string(now_ms()) + "-" + std::to_string(counter++);
   }
   ```
5. Verify that `now_ms()` (which uses `std::chrono::system_clock::now()`) is also thread-safe — it is per the C++ standard.

#### Verification

- [ ] `make clean && make && make test` passes
- [ ] ThreadSanitizer (or `-fsanitize=thread`) run of a concurrent session read/write test shows no data races
- [ ] `grep 'cache_valid_ =' lib/session.cpp` — all sites use `mutable` or are in non-const methods
- [ ] `new_id()` — `static` counter uses `std::atomic<long long>` instead of `long long`

---

### NEW-003 — RAII-wrap CURL handles 🔴 CRITICAL

| Field | Value |
|-------|-------|
| **Files** | `lib/http_transport.cpp`, `lib/http_transport.h` |
| **Severity** | 🔴 Critical |
| **Estimated effort** | 2 hours |

#### Problem

`post_completion()` and `stream_completion()` both use raw `curl_easy_init()` / `curl_easy_cleanup()`:

```cpp
CURL* c = curl_easy_init();
if (!c) throw std::runtime_error("curl_easy_init failed");
// ... curl_easy_setopt calls ...
curl_easy_cleanup(c);
```

If any code between init and cleanup throws (e.g., `std::bad_alloc` from string operations, or an exception from a called function), the `CURL*` handle leaks. There's no RAII guard.

#### Target architecture

Introduce a `CurlHandle` RAII wrapper:

```cpp
// In http_transport.h
struct CurlHandle {
    CURL* handle;
    CurlHandle() : handle(curl_easy_init()) {}
    ~CurlHandle() { if (handle) curl_easy_cleanup(handle); }
    CurlHandle(const CurlHandle&) = delete;
    CurlHandle& operator=(const CurlHandle&) = delete;
    CurlHandle(CurlHandle&& other) noexcept : handle(other.handle) { other.handle = nullptr; }
    CurlHandle& operator=(CurlHandle&& other) noexcept {
        if (handle) curl_easy_cleanup(handle);
        handle = other.handle;
        other.handle = nullptr;
        return *this;
    }
    CURL* get() const { return handle; }
    explicit operator bool() const { return handle != nullptr; }
};
```

Or simpler: `unique_ptr<CURL, decltype(&curl_easy_cleanup)>`.

#### Refactor rules

- Use `unique_ptr<CURL, decltype(&curl_easy_cleanup)>`. The custom deleter is `&curl_easy_cleanup`.
- Replace all `CURL* c = curl_easy_init()` / `curl_easy_cleanup(c)` pairs with this wrapper.
- Both `post_completion` and `stream_completion` must use the wrapper.
- The wrapper must handle the null-from-init case gracefully (throw same `std::runtime_error`).

#### Verification

- [ ] `make clean && make && make test` passes
- [ ] `grep -n 'curl_easy_init\|curl_easy_cleanup' lib/http_transport.cpp` — only the RAII wrapper contains these calls
- [ ] `grep -n 'CURL\*' lib/http_transport.cpp` — no raw CURL pointers outside the wrapper
- [ ] Inject a simulated `std::bad_alloc` after init; verify ASan shows no leak

---

### FIX-001 — Move cancel token into core 🔴 CRITICAL [carry]

Already documented in `docs/fix-tracker.md` as Task 1. Key details:

- **Problem:** `tools/bash_tool.cpp` holds `static std::atomic<bool> g_tool_cancel` accessed by core (`http_transport.cpp`, `tools.h`). Hexagonal boundary violation.
- **Target:** `CancellationToken` in `include/agent/process.h`. Instance-scoped. Owned by `Config`.
- **Status:** Design complete in fix-tracker. Ready for implementation.

### FIX-002 — Fix detached thread in Agent::chat_once 🔴 CRITICAL [carry]

Already documented in `docs/fix-tracker.md` as Task 2.

- **Problem:** `std::thread([this]{...}).detach()` in chat_once — use-after-free if Agent destroyed before thread completes.
- **Target:** Store `std::future` in Agent, join on destruction. Or synchronous extraction (Option B).

---

## Phase 2 — Architecture & code duplication

---

### NEW-004 — Factor HTTP transport (deduplicate post/stream_completion) 🟠 HIGH

| Field | Value |
|-------|-------|
| **Files** | `lib/http_transport.cpp`, `lib/http_transport.h` |
| **Severity** | 🟠 High |
| **Depends on** | NEW-003 (RAII CURL handles) |
| **Estimated effort** | 3-4 hours |

#### Problem

`post_completion()` (lines 108-153) and `stream_completion()` (lines 157-202) share ~90% identical code:

- Curl init, error handling, cleanup
- HeaderList setup (`Content-Type`, `Accept`, auth)
- CURL option wiring (`URL`, `POSTFIELDS`, timeout, `NOPROGRESS`, `XFERINFOFUNCTION/DATA`)
- Response code checking and error formatting
- Timing collection (`ttfb`, `total`)
- Response-capping (`300L` vs `900L`)

The only differences are:
1. Write callback (`write_cb` vs `stream_write_cb`)
2. `Accept` header (optional)
3. Timeout value (300 vs 900)
4. Stats collection (inline vs from StreamParser)
5. Buffer size option for streaming

#### Target architecture

Extract a shared `curl_perform()` helper:

```cpp
struct CurlRequest {
    const Config& cfg;
    const std::string& payload;
    bool accept_sse;
    long timeout_s;
    void* write_data;
    curl_write_callback write_fn;
};

struct CurlResponse {
    std::string body;
    long http_code;
    double ttfb;
    double total;
    CURLcode rc;
};

CurlResponse curl_perform(const CurlRequest& req);
```

Then:

```cpp
std::string post_completion(...) {
    CurlRequest req{cfg, payload, false, 300, &response, LLMClient::write_cb};
    auto resp = curl_perform(req);
    // ... existing logic
}

void stream_completion(...) {
    CurlRequest req{cfg, payload, true, 900, &parser, stream_write_cb};
    // Add CURLOPT_BUFFERSIZE
    auto resp = curl_perform(req);
    // ... existing logic
}
```

#### Refactor rules

- `curl_perform()` must be `noexcept(false)` — it can throw `std::runtime_error` on curl init failure, HTTP errors, etc.
- All CURL option setting that is shared goes into `curl_perform()`.
- Per-call options (buffer size) are set in the caller before calling `curl_perform()`.
- The RAII wrapper from NEW-003 is used inside `curl_perform()`.

#### Verification

- [ ] `make clean && make && make test` passes
- [ ] Both buffered and streaming HTTP calls work (headless CLI, TUI)
- [ ] Error paths (connection refused, HTTP 4xx/5xx) produce the same error messages as before
- [ ] `post_completion()` and `stream_completion()` each ≤15 lines
- [ ] `diff -u <(sed -n '108,153p' old) <(sed -n '108,153p' new)` shows the extracted code is gone

---

### NEW-005 — Fix SessionStore::rebuild_index recursion hazard 🟠 HIGH

| Field | Value |
|-------|-------|
| **Files** | `lib/session.cpp` |
| **Severity** | 🟠 High |
| **Depends on** | NEW-002 (SessionStore fixes) |
| **Estimated effort** | 1-2 hours |

#### Problem

`SessionStore::rebuild_index()` calls `list()` which calls `rebuild_index()` again:

```cpp
void SessionStore::rebuild_index() const {
    auto entries = list();  // calls rebuild_index() if cache was invalid!
    // ...
}
```

This works only because `list()` sets `cache_valid_ = true` BEFORE calling `rebuild_index()`. The ordering is extremely fragile — any future code path that clears the cache between the `list()` call and the index write causes infinite recursion.

#### Target architecture

Break the circular dependency:

```cpp
void SessionStore::rebuild_index() const {
    // Do NOT call list(). Iterate directory directly.
    std::vector<SessionMeta> entries = scan_directory();
    // Build and write index...
}

std::vector<SessionMeta> SessionStore::scan_directory() const {
    // Pure directory scan, no cache, no list() call.
}

std::vector<SessionMeta> SessionStore::list() const {
    if (cache_valid_) return list_cache_;
    list_cache_ = scan_directory();  // no recursion
    cache_valid_ = true;
    rebuild_index();  // now safe: uses cache or scans again (but won't recurse)
    return list_cache_;
}
```

#### Refactor rules

- Extract the directory-scanning logic from `list()` into a private `scan_directory()` method that NEVER calls `rebuild_index()` or `list()`.
- `rebuild_index()` calls `scan_directory()` directly if it needs fresh data, or reads from `list_cache_` if acceptable.
- Remove the early index-file read from `list()` — the index file is a cache optimization, not a correctness mechanism. Simpler to always scan and write the index after.

#### Verification

- [ ] `make clean && make && make test` passes
- [ ] `grep -n 'list()' lib/session.cpp` — `rebuild_index` does not call `list()`
- [ ] Session listing works correctly (index file written, read back on next startup)
- [ ] No infinite recursion even with forced cache invalidation

---

### FIX-003 — Decompose Agent::run() 🟠 HIGH [carry]

Already documented in `docs/fix-tracker.md` as Task 3.

- **Problem:** `Agent::run()` is ~155 lines with 5+ responsibilities. Violates <10-line method rule.
- **Target:** Extract 10+ private methods. Final `run()` must be ≤30 lines. Every extracted method ≤10 lines.
- **Note:** The existing AGENTS.md claims `agent.cpp` was already split (473→200), but `run()` was not part of that split and remains too large.

### FIX-004 — Decompose Agent::compress_now() 🟠 HIGH [carry]

Already documented in `docs/fix-tracker.md` as Task 4.

- **Problem:** `compress_now()` (~160 lines) duplicates the compression pipeline already in `CompressionPipeline::compress()`. Also defines a local `Reporter` class inline.
- **Target:** Reuse `CompressionPipeline::compress()`. Add `CompressionObserver` interface. `compress_now()` body ≤40 lines.

---

### NEW-006 — Extract CompressionReporter from inline class 🟠 HIGH

| Field | Value |
|-------|-------|
| **Files** | `lib/agent.cpp`, `include/agent/compressor.h`, `lib/compressor.cpp` |
| **Severity** | 🟠 High |
| **Depends on** | FIX-004 (CompressionObserver interface) |
| **Estimated effort** | 2 hours |

#### Problem

`Agent::compress_now()` (lines 155-210 in current code) defines a 55-line `Reporter` class as a local class inside the method. This:

1. Cannot be unit-tested in isolation
2. Clutters the method (the reporter is half the method)
3. Violates SRP (compress_now() shouldn't define new types)

#### Target architecture

Extract `CompressionReporter` as a standalone class in `lib/compressor.cpp` / `include/agent/compressor.h`:

```cpp
class CompressionReporter : public CompressionObserver {
public:
    CompressionReporter(const AgentHooks& hooks, CompressionResult& result);
    // Override all CompressionObserver methods...
private:
    const AgentHooks& hooks_;
    CompressionResult& result_;
    std::chrono::steady_clock::time_point t0_;
    void log(const std::string& msg);
};
```

Then `compress_now()` creates the reporter with `std::make_unique<CompressionReporter>(hooks_, r)` and passes it to the pipeline.

#### Refactor rules

- The `Reporter` class moves to the `lib/` namespace (not nested inside `Agent`).
- Its header is exposed in `include/agent/compressor.h` alongside `CompressionObserver`.
- `Agent::compress_now()` instantiates it and passes it to `compression_->compress()`.
- The `log()` helper is public or private as appropriate.

#### Verification

- [ ] `make clean && make && make test` passes
- [ ] `grep -n 'class.*Reporter\|class.*Observer' lib/agent.cpp` — no local Observer/Reporter classes defined inside `agent.cpp`
- [ ] `CompressionReporter` unit test exists in `tests/` (can inject mock `AgentHooks`)

---

## Phase 3 — Extensibility & coupling

---

### NEW-007 — Abstract LLMClient behind interface 🟠 HIGH

| Field | Value |
|-------|-------|
| **Files** | `include/agent/llm.h`, `lib/llm.cpp`, `lib/http_transport.*`, all consumers |
| **Severity** | 🟠 High |
| **Estimated effort** | 4-6 hours |

#### Problem

`LLMClient` is a concrete class. To swap backends (OpenAI → Anthropic → Gemini), you'd need to either:

1. Modify the existing class (violation of OCP)
2. Subclass it (but virtual methods aren't designed for it — `chat()` and `chat_stream()` are not virtual)
3. Create a parallel hierarchy (but Agent takes `LLMClient&`, not an interface)

#### Target architecture

Extract an abstract `LLMBackend` or `IChatClient` interface:

```cpp
// include/agent/llm.h
class IChatClient {
public:
    virtual ~IChatClient() = default;
    virtual ServerInfo probe_server() const = 0;
    virtual Message chat(const std::vector<Message>& messages,
                         const std::vector<Tool*>& tools,
                         Stats* stats = nullptr) = 0;
    virtual Message chat_stream(const std::vector<Message>& messages,
                                const std::vector<Tool*>& tools,
                                const std::function<void(const StreamChunk&)>& on_chunk,
                                Stats* stats = nullptr) = 0;
};
```

Then rename the current `LLMClient` to `OpenAIClient` (or keep `LLMClient` as the concrete implementation) and have `Agent` take `IChatClient&` (or `unique_ptr<IChatClient>`) instead.

#### Refactor rules

- `IChatClient` is a pure interface (all pure virtual, no data members). Lives in `include/agent/llm.h`.
- The existing `LLMClient` becomes an implementation. No changes to its methods are needed beyond adding `override`.
- `Agent` takes `IChatClient&` in its constructor.
- `Agent::chat_once()` calls `client_.chat()` / `client_.chat_stream()` — same as now, since the interface matches.
- Factory functions or a registry can produce backends (optional, not required for this task).
- The `write_cb()` static method stays on the concrete class (or moves to a detail namespace).

#### Verification

- [ ] `make clean && make && make test` passes
- [ ] All existing test/CLI/TUI code compiles without change (interface is a drop-in replacement)
- [ ] A mock implementation of `IChatClient` can be created for hermetic unit tests
- [ ] `grep 'class LLMClient' include/agent/llm.h` — shows both the interface and the concrete class

---

### NEW-008 — Split Config god struct 🟠 HIGH

| Field | Value |
|-------|-------|
| **Files** | `include/agent/config.h`, `lib/config.cpp`, all consumers |
| **Severity** | 🟠 High |
| **Estimated effort** | 6-8 hours (large refactor, touches every file) |

#### Problem

`Config` contains 30+ fields spanning:
- Provider/transport config (api_base, api_key, timeout)
- Model parameters (model, temperature, max_tokens, thinking)
- UI preferences (show_reasoning, mode)
- Detection toggles (detection_loop, detection_duplicate)
- Compression settings (threshold, min_turns, cooldown)
- Experience/memory (experience_enabled, max_memories, max_skills)
- Logging (log_path, debug_log)
- Cancellation (cancel_token)

This is a violation of SRP and ISP. Changes to any concern ripple through every consumer.

#### Target architecture

Split into focused config structs:

```cpp
struct TransportConfig {
    std::string api_base = "http://localhost:8000/v1";
    std::string api_key;
    int timeout_s = 300;
    CancellationToken cancel_token;
};

struct ModelConfig {
    std::string model = "gpt-4o-mini";
    double temperature = 0.2;
    size_t max_tokens = 16384;
    std::string thinking = "auto";
    int thinking_budget = -1;
    std::string reasoning_effort = "off";
    int context_size = 0;
    bool model_explicit = false;
    bool context_explicit = false;
};

struct UIConfig {
    AgentMode mode = AgentMode::Write;
    bool show_reasoning = true;
    bool stream = true;
};

struct DetectionConfig {
    bool detection_loop = true;
    bool detection_duplicate = true;
};

struct CompressionConfig {
    double compression_threshold = 0.0;
    int compression_min_turns = 0;
    int compression_cooldown_turns = 0;
};

struct ExperienceConfig {
    bool experience_enabled = true;
    int experience_max_memories = 0;
    int experience_max_skills = 0;
};

struct LogConfig {
    std::string log_path;
    std::string debug_log;
};

// Aggregator for convenience (not required — consumers take what they need)
struct Config {
    TransportConfig transport;
    ModelConfig model;
    UIConfig ui;
    DetectionConfig detection;
    CompressionConfig compression;
    ExperienceConfig experience;
    LogConfig logging;
    // Provider name and prompt paths remain here (cross-cutting)
    std::string provider_name = "custom";
    std::string system_prompt_path;
    std::string tools_prompt_path;

    std::string api_url() const noexcept;
    std::string models_url() const noexcept;
    void load(const std::string& path);
    // ... other methods stay on the aggregate or move to sub-configs
};
```

#### Refactor rules

- Each sub-config goes in its own header or stays in `config.h` with clear section comments (prefer own headers).
- `Agent` takes individual configs it needs (e.g., `ModelConfig`, `TransportConfig`, `DetectionConfig`), not the entire `Config`.
- `LLMClient` takes `TransportConfig` + `ModelConfig`.
- Tools take only what they need (e.g., `DetectionConfig`).
- `Config::load()` stays as the overall loader that dispatches to sub-configs.
- Update `AGENTS.md` to note that `Config` is an aggregate of focused configs.
- Do this refactor in stages: first split the struct, then narrow constructor parameters.

#### Verification

- [ ] `make clean && make && make test` passes
- [ ] `grep 'cfg_\.' lib/agent.cpp include/agent/agent.h tui/tui.cpp` — verify each consumer only references its relevant sub-config
- [ ] No single `#include "agent/config.h"` pulls in more than 3 sub-config headers
- [ ] The aggregated `Config` can be loaded/saved round-trip (backward compat with existing config files)

---

### FIX-010 — Build system split (core vs tools archives) 🟠 HIGH [carry]

Already documented in `docs/fix-tracker.md` as Task 10.

- **Problem:** `Makefile.in` builds `tools/*.o` into `libagent_core.a`, blurring the hexagonal boundary.
- **Target:** Split into `libagent_core.a` (lib/) + `libagent_tools.a` (tools/).

---

## Phase 4 — Tool & security

---

### NEW-009 — BashTool: implement read-only auto-approval 🟠 HIGH

| Field | Value |
|-------|-------|
| **Files** | `tools/bash_tool.cpp`, `include/agent/tool.h`, `lib/dispatch.cpp` |
| **Severity** | 🟠 High |
| **Depends on** | None |
| **Estimated effort** | 3-4 hours |

#### Problem

`BashTool::requires_approval()` unconditionally returns `true` (line 184), even for read-only commands like `cat`, `ls`, `grep`. The system prompt tells the LLM "Read-only commands (cat, ls, grep, find) need no approval", but this is a **lie** — every bash call gates on user approval. The entire read/write distinction in the approval system is non-functional for the bash tool.

#### Target architecture

Add a `is_read_only_command()` heuristic that checks if the command starts with a read-only command:

```cpp
bool is_read_only_command(const std::string& cmd) {
    // Trim leading whitespace
    // Check first word against a known read-only list:
    //   cat, ls, grep, find, head, tail, wc, sort, uniq, which, type, file, stat, du, df, echo, printf, pwd, date, whoami, id, env, printenv, git status, git log, git diff (without --cached or modifications), etc.
    // Return true only for safe, non-mutating commands.
}
```

The bash tool then returns `requires_approval()` based on this:

```cpp
bool requires_approval() const noexcept override {
    return !args_analyzed_as_read_only_;
}
```

However, since `requires_approval()` has no access to the arguments (it's called before `execute()`), we need a different approach:

**Option A**: Make `requires_approval()` take the arguments:
```cpp
bool requires_approval(const json& args) const override;
```
This requires changing the `Tool` interface and all implementations.

**Option B**: Implement read-only detection in `approve_tool()` in dispatch.cpp, using knowledge of the bash tool's argument structure.

**Option C**: Make bash tool have `is_read_only()` return false (as now), but override `requires_approval` to accept arguments. Or use a two-phase approach: `execute()` can skip the approval gate if the command is detected as read-only internally.

**Preferred: Option A** — change the `Tool` interface to pass args to `requires_approval()`:

```cpp
class Tool {
public:
    virtual bool requires_approval(const json& args) const noexcept;
    virtual bool is_read_only(const json& args) const noexcept;
};
```

Default implementations ignore args (backward compatible). `BashTool` overrides:

```cpp
bool requires_approval(const json& args) const noexcept override {
    if (!args.contains("command") || !args["command"].is_string()) return true;
    return !is_read_only_command(args["command"].get<std::string>());
}
```

#### Refactor rules

- `is_read_only_command()` is a free function in the anonymous namespace of `bash_tool.cpp`.
- The read-only command list: `cat, ls, grep, find, head, tail, wc, sort, uniq, which, type, file, stat, du, df, echo, printf, pwd, date, whoami, id, env, printenv, git status, git log, git diff` (non-mutating forms only).
- Pipes and redirects: `cat foo | grep bar` is still read-only. `cat > file` is not. Simple heuristic: disallow `>` and `>>` and `|` to a command that writes (impractical). Better: just check if the first word is in the read-only list; piping `cat` through `grep` is still read-only at the command level.
- Update `dispatch.cpp` to pass args to `requires_approval(args)` instead of `requires_approval()`.
- All existing tool overrides of `requires_approval()` without args get a `(const json&)` parameter added.

#### Verification

- [ ] `make clean && make && make test` passes
- [ ] `BashTool("ls -la")` — `requires_approval()` returns `false`
- [ ] `BashTool("rm -rf /")` — `requires_approval()` returns `true`
- [ ] `BashTool("cat foo | grep bar")` — `requires_approval()` returns `false` (starts with cat)
- [ ] `BashTool("sed -i 's/foo/bar/' file")` — `requires_approval()` returns `true`
- [ ] All other tools (search, read, write, process_*) still work with correct approval semantics

---

### NEW-010 — dispatch_tool_calls: parallel futures with out-of-order collection 🟡 MEDIUM

| Field | Value |
|-------|-------|
| **Files** | `lib/dispatch.cpp`, `include/agent/dispatch.h` |
| **Severity** | 🟡 Medium |
| **Estimated effort** | 3-4 hours |

#### Problem

`dispatch_tool_calls()` launches all tool calls via `std::async` (parallel), but collects results **in order**:

```cpp
futures[i] = std::async(std::launch::async, [&todo, i]() { ... });
// ...
for (size_t i = 0; i < todo.size(); ++i) {
    res = futures[i].get();  // blocks on index 0 even if index 2 finished first
}
```

A fast tool call (e.g., `read` in 1ms) cannot be shown to the model until all slower tools complete (e.g., `bash` running for 60s). The UI can't stream partial results.

#### Target architecture

Use a completion-ordered collection:

```cpp
struct PendingCall {
    size_t index;
    std::future<ToolResult> future;
};
std::vector<PendingCall> pending;
for (size_t i = 0; i < todo.size(); ++i) {
    if (todo[i].approved) {
        pending.push_back({i, std::async(std::launch::async, [&todo, i]() { ... })});
    }
}

// Process as completed
while (!pending.empty()) {
    for (auto it = pending.begin(); it != pending.end(); ) {
        if (it->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            ToolResult res = it->future.get();
            size_t idx = it->index;
            auto& c = todo[idx];
            // Process result: hooks, log, push to history
            pending.erase(it);
            break;  // restart scan to avoid iterator invalidation
        } else {
            ++it;
        }
    }
}
```

#### Refactor rules

- The outer loop over `todo` (result processing) must be factored into a helper that takes a single completed call and processes it.
- The busy-wait polling loop (`wait_for(0)`) is acceptable for this use case (tool results are infrequent). Use a small `usleep` to avoid tight spinning: `std::this_thread::sleep_for(std::chrono::milliseconds(10))` between scans if no results are ready.
- If C++20 is available, use `std::when_any` pattern with shared state. Otherwise, the polling approach is fine.
- Maintain the invariant that tool results are appended to `history_` in the order they COMPLETE, not the order they were called.

#### Verification

- [ ] `make clean && make && make test` passes
- [ ] When two tool calls are dispatched, the faster one's result appears in history before the slower one's
- [ ] All error/approval/denied paths still produce correct output
- [ ] No data races (TSan clean)

### FIX-011 — Remove naive memory extraction heuristic 🟡 MEDIUM [carry]

Already documented in `docs/fix-tracker.md` as Task 11.

- **Problem:** Length-based heuristic (50 < size < 5000) captures command output as "memories".
- **Target:** Remove the heuristic extraction entirely (Option A). The LLM-based extraction in `compress_now()` is the correct path.

---

## Phase 5 — Testing & CI

---

### NEW-011 — Migrate to Catch2 test framework 🟡 MEDIUM

| Field | Value |
|-------|-------|
| **Files** | `tests/test_util.h`, `tests/run_tests.cpp`, `Makefile.in` |
| **Severity** | 🟡 Medium |
| **Estimated effort** | 4-6 hours |

#### Problem

The project uses a hand-rolled test framework (~90 lines in `test_util.h`) with no:
- Parameterized tests
- Test fixtures / setup/teardown
- Matchers or rich assertions
- Test filtering or selective runs
- XML/JUnit output for CI

The stated reason ("dependency-free for minimal Linux servers") is weak — Catch2 is available as a single header (`catch_amalgamated.hpp`) or via FetchContent.

#### Target architecture

Replace `test_util.h` with Catch2 v3 (header-only):

```cpp
// tests/test_main.cpp
#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

// tests/config_test.cpp
TEST_CASE("config defaults", "[config]") {
    agent::Config c;
    REQUIRE(c.api_base == "http://localhost:8000/v1");
    REQUIRE(c.model == "gpt-4o-mini");
}

TEST_CASE("config validate rejects bad values", "[config]") {
    agent::Config c;
    c.model = "";
    c.api_base = "localhost:8000";  // missing scheme
    auto errs = c.validate();
    REQUIRE_FALSE(errs.empty());
}
```

#### Refactor rules

- Keep the existing test macros (`TEST`, `ASSERT`, `ASSERT_EQ`) as a thin compatibility shim if desired, but migrate all new tests to Catch2.
- Rewrite all existing tests to use Catch2 syntax.
- Break the single 2121-line test file into per-module files: `tests/config_test.cpp`, `tests/workspace_test.cpp`, `tests/session_test.cpp`, `tests/tools_test.cpp`, `tests/dispatch_test.cpp`, `tests/compressor_test.cpp`, `tests/agent_test.cpp`, etc.
- Update `Makefile.in`'s `UNITTEST_OBJ` to include the new files.
- Catch2 is fetched via CMake's FetchContent or the amalgamated header is committed to `third_party/`.

#### Verification

- [ ] `make clean && make && make test` passes
- [ ] All 100+ existing tests pass with identical coverage
- [ ] `make test` supports `--list-tests` and `--verbosity high` via Catch2
- [ ] CI output includes JUnit XML for test reporting
- [ ] `grep -rn 'TEST(' tests/` shows 0 uses of the old `TEST` macro (or the shim is clearly marked as deprecated)

---

### NEW-012 — Add integration tests for Agent::run() 🟡 MEDIUM

| Field | Value |
|-------|-------|
| **Files** | New `tests/agent_test.cpp`, `tests/mock_llm.h` |
| **Severity** | 🟡 Medium |
| **Estimated effort** | 4-6 hours |

#### Problem

The agent loop (`Agent::run()`) has **zero test coverage**. The most critical code path in the entire application is untested. Bug fixes in the loop cannot follow the TDD workflow (Red → Green) because no test infrastructure exists.

#### Target architecture

Create a mock LLM backend and a mock tool for hermetic agent testing:

```cpp
// tests/mock_llm.h
class MockLLM : public IChatClient {
public:
    // Pre-defined response sequences
    std::vector<Message> responses;
    size_t index = 0;

    Message chat(const std::vector<Message>&, const std::vector<Tool*>&, Stats*) override {
        Message m = responses[index % responses.size()];
        ++index;
        return m;
    }
    // ...
};
```

Test cases:

```cpp
TEST_CASE("Agent completes single turn without tools", "[agent]") {
    MockLLM llm;
    llm.responses = {Message{"assistant", "Hello, how can I help?"}};
    // ... setup agent with mock
    auto result = agent.run("Hello");
    REQUIRE(result == "Hello, how can I help?");
}

TEST_CASE("Agent dispatches tool calls and incorporates results", "[agent]") {
    // Two-turn: tool call then done
    MockLLM llm;
    json tc;
    tc["id"] = "call_1";
    tc["function"]["name"] = "mock_tool";
    tc["function"]["arguments"] = R"({"key": "value"})";
    Message with_tool;
    with_tool.role = "assistant";
    with_tool.tool_calls = {tc};
    Message done;
    done.role = "assistant";
    done.content = "done.";
    llm.responses = {with_tool, done};
    // ... setup registry with mock tool, run
    REQUIRE(result == "done.");
}

TEST_CASE("Agent detects tool loop and breaks", "[agent]") {
    // Same tool call 3 times → loop detection fires
}

TEST_CASE("Agent detects text loop and injects recovery steer", "[agent]") {
    // Same response 2+ times → recovery steer injected
}

TEST_CASE("Agent respects max_tool_iterations", "[agent]") {
    // Infinite tool calls → eventually stops with empty reply
}
```

#### Refactor rules

- Tests go in a new `tests/agent_test.cpp` (no header).
- Mock classes go in `tests/mock_llm.h` and `tests/mock_tool.h`.
- The mock LLM requires `IChatClient` interface (see NEW-007). If NEW-007 is not done first, the mock can still test via `LLMClient` by not connecting to a real server and catching the transport error — but interface abstraction is preferred.
- At minimum, 5 test cases covering: single-turn, tool call + done, loop detection ×2, max iterations.

#### Verification

- [ ] `make clean && make && make test` passes
- [ ] All 5+ agent tests pass without network access (hermetic)
- [ ] Each test exercises exactly one behaviour (not a multi-scenario test)

---

### NEW-013 — Add unit tests for compression, dispatch, tool recovery 🟡 MEDIUM

| Field | Value |
|-------|-------|
| **Files** | New `tests/compressor_test.cpp`, `tests/dispatch_test.cpp`, `tests/tool_recovery_test.cpp` |
| **Severity** | 🟡 Medium |
| **Estimated effort** | 3-4 hours |

#### Problem

The following critical modules have **no unit test coverage**:
- Compression pipeline (`lib/compressor.cpp`, `compressor_*.cpp`)
- Tool dispatch (`lib/dispatch.cpp`)
- Tool recovery / FailStreak (`lib/tool_recovery.cpp`)

#### Target architecture

Add tests for:

**Compression:**
- `parse_compression_response()` with valid/invalid/malformed JSON
- `build_compression_request()` produces expected prompt
- `compress_prompt_loop()` detects repeated tool calls
- `apply_classification()` correctly filters messages
- `CompressionGate::should_compress()` at threshold boundaries
- `CompressionPipeline::compress()` end-to-end (with mock LLM)

**Dispatch:**
- `find_duplicate_call()` detects identical calls, allows different calls
- `approve_tool()` with/without `on_approval` hook, session vs once
- `dispatch_tool_calls()` with approved/denied/malformed calls
- `dispatch_tool_calls()` with mixed success/failure results

**Tool Recovery:**
- `FailStreak::update()` with consecutive failures
- `FailStreak::update()` with interleaved success
- `inject_tool_recovery_steer()` produces expected steer message

#### Verification

- [ ] `make clean && make && make test` passes
- [ ] ≥15 new test cases across the 3 modules
- [ ] Line coverage for the tested modules ≥80% (`gcov` or similar)

---

### FIX-005 — Decouple test suite from TUI headers 🟡 MEDIUM [carry]
### FIX-012 — noexcept correctness 🔵 LOW [carry]
### FIX-013 — opendir → filesystem 🔵 LOW [carry]
### FIX-014 — Workspace test isolation 🔵 LOW [carry]

All documented in `docs/fix-tracker.md`.

---

## Phase 6 — Build & tooling

---

### NEW-014 — Replace hand-rolled configure with CMake 🟠 HIGH

| Field | Value |
|-------|-------|
| **Files** | `CMakeLists.txt` (new), `configure` (deprecate), `Makefile.in` (deprecate), `GNUmakefile` (update) |
| **Severity** | 🟠 High |
| **Estimated effort** | 8-12 hours |

#### Problem

The project uses a hand-written shell script (`configure`, 174 lines) that:
- Manually detects CURL, ncurses, md4c
- Generates `Makefile` from `Makefile.in` via `sed` substitution
- Has no built-in `compile_commands.json` generation
- Requires editing in 3 places for a new source file (`Makefile.in` pattern + object variable + dependency line)
- No support for cross-compilation, install prefixes, or package management
- `compile_flags.txt` is out of sync with the real build

#### Target architecture

```cmake
# CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(amber VERSION 0.3.0 LANGUAGES C CXX)

find_package(CURL REQUIRED)
find_package(Curses REQUIRED)

add_library(agent_core STATIC
    lib/config.cpp lib/llm.cpp lib/http_transport.cpp
    lib/agent.cpp lib/agent_helpers.cpp
    # ... all lib/*.cpp
)
target_include_directories(agent_core PUBLIC include)
target_link_libraries(agent_core PUBLIC CURL::libcurl)
target_compile_features(agent_core PUBLIC cxx_std_17)

add_library(agent_tools STATIC
    tools/bash_tool.cpp tools/search_tool.cpp
    tools/read_tool.cpp tools/write_tool.cpp
    tools/process_tool.cpp
    tools/search/grep_backend.cpp
    tools/search/semantic_backend.cpp
    tools/search/semantic_index.cpp
)
target_include_directories(agent_tools PUBLIC include tools)
target_link_libraries(agent_tools PUBLIC agent_core)

add_executable(amber src/main.cpp)
target_link_libraries(amber agent_core agent_tools)

add_executable(amber-tui
    tui/tui.cpp tui/tui_render.cpp tui/tui_input.cpp
    # ... all tui/*.cpp, third_party/md4c/md4c.c
)
target_link_libraries(amber-tui agent_core agent_tools Curses::Curses Curses::Form Curses::Menu Curses::Panel)

# Enable clang-tidy via CMAKE_CXX_CLANG_TIDY
# Generate compile_commands.json automatically
# Install targets
```

Benefits:
- Automatic `compile_commands.json` for clangd
- `find_package(CURL)` replaces the hand-rolled detection
- Adding a new source file: just add it to the CMakeLists.txt list
- Cross-platform out of the box
- `cmake --build . --target lint` via `CMAKE_CXX_CLANG_TIDY`
- `CTest` integration for `make test`

#### Refactor rules

- Keep the autotools `Makefile.in` / `configure` / `GNUmakefile` for backward compatibility during transition.
- Add `CMakeLists.txt` at the root. Add `cmake_minimum_required(VERSION 3.20)`.
- Use `CMAKE_CXX_STANDARD 17` and `CMAKE_CXX_STANDARD_REQUIRED ON`.
- Generate `version.h` from `version.h.in` via `configure_file()`.
- Add a `CPack` section for packaging.
- Update `GNUmakefile` to detect CMake and delegate, or remove it.
- Update CI (`ci.yml`) to build with CMake.

#### Verification

- [ ] `cmake -B build && cmake --build build` produces all 4 targets (amber, amber-tui, libagent_core.a, libagent_tools.a)
- [ ] `ctest --test-dir build` runs all unit tests and passes
- [ ] `compile_commands.json` is generated in the build dir
- [ ] `clang-tidy` can read `compile_commands.json` from the build dir
- [ ] `make install` (via cmake --install) installs binaries and headers to the prefix
- [ ] The old `./configure && make` path still works (non-breaking transition)

---

### NEW-015 — Un-suppress clang-tidy checks 🟡 MEDIUM

| Field | Value |
|-------|-------|
| **Files** | `.clang-tidy` |
| **Severity** | 🟡 Medium |
| **Estimated effort** | 4-6 hours (iterative) |

#### Problem

`.clang-tidy` suppresses 17 checks, including:
- `bugprone-narrowing-conversions` — catches actual bugs
- `performance-unnecessary-copy-initialization` — performance regressions
- `modernize-use-nodiscard` — prevents ignoring return values
- `readability-function-cognitive-complexity` — would flag the god methods
- `modernize-avoid-c-arrays` — C-compatibility issues

The suppression list is longer than the enabled list. The config was tuned to pass CI rather than improve code quality.

#### Target architecture

Phase in strictness:

1. First pass: re-enable `bugprone-narrowing-conversions`, `performance-unnecessary-copy-initialization`, `modernize-use-nodiscard`.
2. Fix all violations across the codebase.
3. Second pass: re-enable `modernize-avoid-c-arrays`.
4. Third pass: re-enable `readability-function-cognitive-complexity` with a threshold of 25.
5. After major refactors (FIX-003, NEW-006, etc.), re-enable all remaining suppressions.

#### Refactor rules

- Each re-enablement is a separate commit with the corresponding fixes.
- The goal is to reach a `.clang-tidy` with zero `-*` suppressions (or only those that conflict with an explicit project convention).
- Document any remaining suppression with a comment explaining why.

#### Verification

- [ ] `make lint` passes with zero warnings at each stage
- [ ] The `.clang-tidy` suppression list is ≤5 items (down from 17)
- [ ] Each suppressed check has a comment explaining why it cannot be enabled

---

### NEW-016 — Sync compile_flags.txt with real build 🔵 LOW

| Field | Value |
|-------|-------|
| **Files** | `compile_flags.txt` |
| **Severity** | 🔵 Low |
| **Estimated effort** | 30 minutes |

#### Problem

`compile_flags.txt` is "minimal" per AGENTS.md. clangd uses it for LSP, meaning IDE diagnostics may differ from `make` diagnostics.

#### Fix

1. Generate `compile_flags.txt` from the build system (or update it to match):
   ```
   -std=c++17
   -Iinclude
   -Isrc
   -Itools
   -I.
   -I/usr/include
   ```
2. OR if using CMake (NEW-014), rely on `compile_commands.json` instead and remove `compile_flags.txt`.
3. Document in AGENTS.md that clangd reads `compile_commands.json` from the build directory.

#### Verification

- [ ] clangd in the repo root produces the same diagnostics as `make lint`
- [ ] `compile_flags.txt` (if kept) matches the flags used by the build

---

### NEW-017 — Move vendored deps to submodules / FetchContent 🔵 LOW

| Field | Value |
|-------|-------|
| **Files** | `.gitmodules`, `CMakeLists.txt`, `include/nlohmann/json.hpp`, `third_party/md4c/` |
| **Severity** | 🔵 Low |
| **Estimated effort** | 1-2 hours |

#### Problem

`include/nlohmann/json.hpp` (24,765 lines) and `third_party/md4c/` (7,718 lines) are committed to the repo. This doubles the repo size, makes security scanning harder, and complicates updates.

#### Fix

1. Add nlohmann/json as a git submodule or CMake FetchContent dependency.
2. Add md4c as a git submodule or vendored copy (it's small, less critical).
3. Update `#include` paths if needed.
4. Update `THIRD_PARTY_LICENSES` file.
5. Update CI workflows to fetch submodules.

#### Verification

- [ ] `git clone --recurse-submodules <repo>` builds successfully
- [ ] `cmake -B build && cmake --build build` works with fetched dependencies
- [ ] LICENSE metadata files are updated
- [ ] The repo size decreases by ~32k lines (exclude the vendored files)

---

### FIX-006 — Lighten dispatch.h includes 🟡 MEDIUM [carry]

Already documented in `docs/fix-tracker.md` as Task 6.

---

## Phase 7 — Documentation

---

### NEW-018 — Remove umbrella include/agent.h, enforce IWYU 🔵 LOW

| Field | Value |
|-------|-------|
| **Files** | `include/agent.h`, all files that include it |
| **Severity** | 🔵 Low |
| **Estimated effort** | 2-3 hours |

#### Problem

`include/agent.h` includes 20+ headers. It's included by `tui/tui.cpp` and other files that only need a subset. This violates the Include What You Use principle and causes unnecessary recompilation.

#### Fix

1. Remove `include/agent.h` (or deprecate it).
2. Replace every `#include "agent.h"` with the specific headers needed.
3. Enforce IWYU in CI via `clang-tidy --checks=-*,misc-include-cleaner` (available in clang-tidy 17+).

#### Verification

- [ ] `make clean && make && make test` passes
- [ ] `grep -rn '#include "agent.h"' lib/ tui/ tools/ src/ tests/` returns 0
- [ ] `clang-tidy --checks=-*,misc-include-cleaner` reports 0 findings on the project source

---

### FIX-007, FIX-008, FIX-009 — Documentation updates 🔵 LOW [carry]

Already documented:
- FIX-007: Document design patterns in AGENTS.md
- FIX-008: Add TDD/Red-Green policy
- FIX-009: Add code review and error handling conventions

---

## Implementation ordering

### Phase 1a (week 1): Safety first
```
NEW-001  Signal handler UB
NEW-002  SessionStore const + data race
NEW-003  RAII CURL handles
FIX-001  Cancel token in core
FIX-002  Detached thread fix
```

### Phase 1b (week 2): Architecture
```
NEW-004  Factor HTTP transport
NEW-005  SessionStore recursion hazard
NEW-006  Extract CompressionReporter
FIX-003  Decompose Agent::run()
FIX-004  Decompose Agent::compress_now()
```

### Phase 2 (week 3): Extensibility
```
NEW-007  Abstract LLMClient
NEW-008  Split Config (incremental)
FIX-010  Build system split
```

### Phase 3 (week 4): Tools and dispatch
```
NEW-009  BashTool read-only auto-approval
NEW-010  Out-of-order tool result collection
FIX-011  Remove memory heuristic
```

### Phase 4 (week 5-6): Testing
```
NEW-011  Catch2 migration
NEW-012  Agent integration tests
NEW-013  Compression/dispatch/recovery tests
FIX-005  Decouple tests from TUI
```

### Phase 5 (week 7): Build & tooling
```
NEW-014  CMake migration
NEW-015  Un-suppress clang-tidy
NEW-016  Sync compile_flags.txt
NEW-017  Vendored deps → submodules
```

### Phase 6 (parallel, ongoing):
```
FIX-006  Lighten dispatch.h
FIX-012  noexcept correctness
FIX-013  opendir → filesystem
FIX-014  Workspace isolation
NEW-018  Remove umbrella header
FIX-007/008/009  Documentation
```

---

## Effort summary

| Phase | Tasks | Estimated effort | Risk |
|-------|-------|-----------------|------|
| 1a Safety | 5 tasks | 12-15 hours | Low (narrow scope) |
| 1b Architecture | 5 tasks | 15-20 hours | Medium (refactors touch core) |
| 2 Extensibility | 3 tasks | 20-25 hours | High (Config split touches every file) |
| 3 Tools | 3 tasks | 10-12 hours | Medium (approval logic change) |
| 4 Testing | 4 tasks | 18-22 hours | Medium (catch2 migration is large) |
| 5 Build | 4 tasks | 15-20 hours | High (CMake migration is disruptive) |
| 6 Docs/Low | 7 tasks | 8-12 hours | Low |
| **Total** | **31 tasks** | **~100 hours** | |

---

## Risk mitigations

1. **Phase ordering protects critical path**: safety issues fixed first, before any architectural refactor.
2. **Each task is independently testable**: every task has a verification checklist that must pass before sign-off.
3. **Incremental Config split**: split one sub-config at a time rather than attempting a monolithic refactor.
4. **Parallel documentation**: FIX-007/008/009 can be done by anyone at any time without code risk.
5. **Backward compat for CMake**: keep the autotools build alive during the transition period (3-4 weeks).
6. **Test-first for critical paths**: Agent integration tests (NEW-012) prevent regressions during the `Agent::run()` decomposition (FIX-003).
