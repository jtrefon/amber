# amber — Issue Fix Tracker

- **Status:** ✅ Complete (all tasks done)
- **Target:** Zero technical debt
- **Rule:** No patches. Every fix is a **refactor** that leaves the code in a strictly better state than found.
- **Reference:** Issues register at `docs/issues.md`

---

## How to Use This Tracker

1. Every fix follows the **Red → Proposal → Sign-off → Green → PR** workflow
   (see AGENTS.md). On a fresh branch named `<type>/<short-description>`:
   - **Red**: Write a failing test first, commit it.
   - **Proposal**: Link to `docs/fix-tracker.md` or inline the architecture spec.
   - **Sign-off**: Reviewer approves the proposed architecture.
   - **Green**: Implement the fix; make the test pass; refactor to zero debt.
   - **PR**: Open/update the pull request. All checks must pass.
2. Each task below is **self-contained** — an agent or developer can pick it up
   independently.
3. Tasks are ordered by dependency (prerequisites first).
4. Each task has a **refactor spec** describing the target architecture, not just
   the diff.
5. **Verification** must pass before marking a task `[done]`:
   `make clean && make && make test && make lint && make analyze`.
6. If the task touches headers, run `make clean && make` to regenerate `.d` files
   (see AGENTS.md compilation gotchas).
7. Never commit commented-out code, dead branches, or stubs. If the refactor
   removes functionality, remove it entirely.

---

## Legend

```
[done]  — Not started, ready for assignment
[done] — Assigned and actively being worked
[done]     — Code merged, all checks pass, no known regressions
[done]  — Blocked on another task or external dependency
```

---

## Task 1: Move tool-cancel into core (prerequisite for C2, H3)

| Field | Value |
|---|---|
| **ID** | `FIX-001` |
| **Severity** | 🔴 Critical |
| **Depends on** | None |
| **Blocks** | FIX-003 (HTTP transport cleanup) |
| **Estimated effort** | 2-3 hours |
| **Files touched** | `include/agent/tools.h`, `include/agent/process.h` (new or extended), `lib/process.cpp`, `tools/bash_tool.cpp` |

### Problem

`tools/bash_tool.cpp` holds file-scoped `static std::atomic<bool> g_tool_cancel` that is accessed by:
- `include/agent/tools.h` → `request_tool_cancel()` / `is_tool_cancel_requested()` / `clear_tool_cancel()`
- `lib/http_transport.cpp` → `cancel_check_cb` calls `is_tool_cancel_requested()`

This is a hexagonal boundary violation (core → adapter), uses global state, and prevents instance-scoped cancellation.

### Target Architecture

1. Introduce a `CancellationToken` type in the core:
   - File: `include/agent/process.h`
   - `struct CancellationToken { std::shared_ptr<std::atomic<bool>> flag; ... }`
   - Default-constructs with a new `std::atomic<bool>` set to `false`
   - `request()` sets the flag, `is_requested()` reads it, `clear()` resets it
   - Copyable: copies share the same underlying flag
2. Add `CancellationToken cancel_token` field to `Config` (host-owned, injected at Agent construction).
3. Replace all three free functions (`request_tool_cancel`, `is_tool_cancel_requested`, `clear_tool_cancel`) with the token.
4. `http_transport.cpp` receives the token via Config or as an explicit parameter.
5. Remove the globals from `bash_tool.cpp`.

### Refactor Rules

- Do NOT keep the old free functions as wrappers. Remove them entirely.
- The `Config` struct gains a `CancellationToken cancel_token` field (mutable, not const).
- `cancel_check_cb` in `http_transport.cpp` reads `cfg.cancel_token.is_requested()`.
- `BashTool::execute()` and `process_start_tool` check the same token.
- The TUI (`/stop` command, Esc key) calls `cfg_.cancel_token.request()` instead of `request_tool_cancel()`.
- _Do not_ add `#include <atomic>` to `Config.h` if not already present — forward-declare or include `process.h`.

### Verification

- [ ] `make clean && make && make test` passes
- [ ] `make lint && make analyze` clean
- [ ] `grep -rn 'g_tool_cancel' tools/ lib/ include/` returns nothing
- [ ] `grep -rn 'request_tool_cancel\|is_tool_cancel_requested\|clear_tool_cancel'` returns nothing
- [ ] TUI Esc during streaming or bash execution cancels within 1-2 seconds
- [ ] Two `Agent` instances (if ever created) have independent cancellation

---

## Task 2: Fix detached thread in Agent::chat_once (C1)

| Field | Value |
|---|---|
| **ID** | `FIX-002` |
| **Severity** | 🔴 Critical |
| **Depends on** | None (independent of FIX-001) |
| **Blocks** | Nothing |
| **Estimated effort** | 2-3 hours |
| **Files touched** | `lib/agent.cpp`, `include/agent/agent.h` |

### Problem

`lib/agent.cpp:140-161` spawns a `std::thread([this]{ ... }).detach()` that accesses `memory_store_`, `experience_cfg_`, and `last_extraction_`. If `Agent` is destroyed before completion, this is use-after-free.

### Target Architecture

Replace the fire-and-forget async extraction with a safe asynchronous pattern. Options ranked by preference:

**Option A (Preferred): Tracked future**
- Store a `std::vector<std::future<void>> pending_extractions_` in `Agent`.
- `chat_once` launches the extraction via `std::async(std::launch::async, ...)` and stores the future.
- `Agent::~Agent()` (or a `shutdown()` method) waits on all pending futures before returning.
- The thread captures `shared_from_this()` or copies the data it needs (value semantics for `ExperienceConfig`, raw pointer to `MemoryStore` with a shared lifetime).

**Option B (Simpler): Synchronous extraction**
- Remove the `std::thread` entirely. Run extraction synchronously after `chat_once` returns, before the next iteration.
- Measure: if extraction takes <50ms for typical history, the UX impact is negligible.
- If performance is a concern, gate extraction behind a config flag.

### Refactor Rules

- Prefer Option A: store `std::future` in Agent, wait on destruction.
- Do NOT use `std::thread::detach()` anywhere in the codebase after this fix.
- Do NOT add `shared_from_this` to `Agent` unless truly needed — prefer value-capture of data.
- If using `std::async`, verify the extraction lambda does not capture `this`; capture `memory_store_` raw pointer only after verifying lifetime is managed (e.g., Agent owns the store via `unique_ptr`, and the future is joined before the `unique_ptr` is destroyed).
- Extract the inline heuristic into a named private method `extract_memories_from_tool_results()`.

### Verification

- [ ] `make clean && make && make test` passes
- [ ] `grep -rn '\.detach()' lib/ tui/ tools/` returns nothing
- [ ] TUI `/quit` during streaming returns within 2 seconds (no blocking on zombie threads)
- [ ] Valgrind or ASan run shows no use-after-free on normal exit

---

## Task 3: Decompose Agent::run() (H1)

| Field | Value |
|---|---|
| **ID** | `FIX-003` |
| **Severity** | 🟠 High |
| **Depends on** | FIX-001 (cancel token) — optional but reduces touch conflicts |
| **Blocks** | Nothing |
| **Estimated effort** | 4-6 hours |
| **Files touched** | `lib/agent.cpp`, `include/agent/agent.h`, optionally `include/agent/agent_helpers.h` + `lib/agent_helpers.cpp` |

### Problem

`Agent::run()` (lib/agent.cpp:383-538) is ~155 lines with 5+ distinct responsibilities. It violates the documented <10-line method rule and SRP.

### Target Architecture

Decompose `run()` into a sequence of named private methods, each ≤10 lines with minimal branching. The final `run()` should read as a linear script:

```cpp
std::string Agent::run(const std::string& user_prompt) {
    ensure_system_prompt();
    log_user_message(user_prompt);
    push_user_message(user_prompt);
    
    auto tools = resolve_tools();
    auto chat_fn = [this, &tools]() { return chat_once(tools); };
    
    FailStreak fail_streak;
    
    for (int iter = 0; iter < cfg_.max_tool_iterations; ++iter) {
        Message reply = safe_chat_once_wrapped(hooks_, log_, chat_fn, "generation");
        history_.push_back(reply);
        log_reasoning(reply);
        maybe_extract_text_tool_calls_from(reply);
        
        if (has_tool_calls(reply)) {
            handle_assistant_text(reply);
            bool ok = dispatch_and_log(reply.tool_calls);
            if (detect_and_handle_loop(reply, ok, fail_streak))
                break;
            continue;
        }
        
        if (detect_and_handle_text_loop(reply))
            break;
        
        std::string accepted = confirm_and_finalize(reply, tools);
        if (!accepted.empty())
            return finalize(accepted);
    }
    
    return finalize(empty_turn_fallback());
}
```

### Extract These Private Methods

| Method | Lines extracted | Responsibility |
|---|---|---|
| `log_user_message(prompt)` | 2 | Log the user event |
| `push_user_message(prompt)` | 4 | Push Message to history_ |
| `resolve_tools()` | 3 | Build `vector<Tool*>` from registry |
| `build_chat_fn(tools)` | 1 | Wrap `chat_once` in lambda |
| `has_tool_calls(reply)` | 1 | Check reply.tool_calls non-null and non-empty |
| `handle_assistant_text(reply)` | 2 | Call hooks_.on_assistant if present |
| `dispatch_and_log(calls)` | 5 | Call dispatch_tool_calls, log result |
| `detect_text_loop(reply)` | 20 | Text loop detection logic (currently inline) |
| `detect_tool_loop(reply, ok, fail_streak)` | 25+ | Tool loop detection + recovery |
| `confirm_and_finalize(reply, tools)` | 10 | confirm_turn + log |
| `finalize(reply)` | 5 | Set state to Idle, log turn_end |

### Refactor Rules

- Every extracted method must be ≤10 lines. If the extracted logic is longer (e.g., loop detection), extract sub-helpers.
- Do NOT introduce new state in `Agent`. All extracted methods use existing members.
- `detect_tool_loop` and `detect_text_loop` are candidates for extraction into `agent_helpers.h` as free functions — consider that if they grow further.
- After extraction, `run()` must be ≤30 lines. Count the lines.
- The loop invariants must hold: `history_` is always in a consistent state, and every path through the loop either continues, breaks with a final reply, or hits max iterations.

### Verification

- [ ] `make clean && make && make test` passes
- [ ] `make lint` clean (no new clang-tidy warnings)
- [ ] `Agent::run()` body ≤30 lines (measure between `{` at line 1 and the first `}` that returns)
- [ ] All extracted methods ≤10 lines (exceptions: `detect_text_loop`, `detect_tool_loop` may be up to 20 if further decomposed)
- [ ] Manual: run `./amber --prompt "hello"` (headless) and verify single-turn reply
- [ ] Manual: run `./amber --prompt "run ls then say done"` and verify two-turn tool use

---

## Task 4: Decompose Agent::compress_now() and deduplicate pipeline (H2)

| Field | Value |
|---|---|
| **ID** | `FIX-004` |
| **Severity** | 🟠 High |
| **Depends on** | FIX-003 (same file — do after to avoid merge conflict) |
| **Blocks** | Nothing |
| **Estimated effort** | 4-6 hours |
| **Files touched** | `lib/agent.cpp`, `include/agent/compressor.h`, `lib/compressor.cpp` |

### Problem

`Agent::compress_now()` (~160 lines) duplicates the compression pipeline already implemented in `CompressionPipeline::compress()` (lib/compressor.cpp:64-103). Both methods do: loop collapse, build request, LLM call, parse, apply classification. The Agent version additionally handles memory/skill ops and status reporting.

### Target Architecture

1. Reuse `CompressionPipeline::compress()` from within `compress_now()`. The pipeline already returns the compressed `vector<Message>`.
2. Extract a `CompressionResult` builder that computes statistics from before/after.
3. Move memory/skill ops application into `CompressionStrategy` or a separate `CompressionObserver` interface.
4. Move status reporting into a separate `CompressionReporter` class or use `AgentHooks.on_status` directly.

### Steps

1. Add a `CompressionObserver` interface to `compressor.h`:

```cpp
struct CompressionObserver {
    virtual ~CompressionObserver() = default;
    virtual void on_compress_start(size_t msgs_before, size_t tokens_before) {}
    virtual void on_loop_collapse(size_t removed) {}
    virtual void on_llm_request_sent() {}
    virtual void on_llm_reply_received(long elapsed_ms) {}
    virtual void on_parse_result(const CompressionResponse& cr) {}
    virtual void on_apply_result(const CompressionResult& r) {}
    virtual void on_memory_ops_applied(size_t upsert, size_t deprecate) {}
    virtual void on_skill_ops_applied(size_t upsert, size_t deprecate) {}
    virtual void on_compress_done(const CompressionResult& r) {}
    virtual void on_error(const std::string& msg) {}
};
```

2. `CompressionPipeline::compress()` takes an optional `CompressionObserver*`:

```cpp
std::vector<Message> compress(
    const std::vector<Message>& history,
    const CompressionConfig& cfg,
    LLMClient& client,
    CompressionObserver* observer = nullptr);
```

3. `Agent::compress_now()` becomes:

```cpp
CompressionResult Agent::compress_now() {
    if (!compression_) return {};
    if (history_.size() < 2) return {};
    
    auto reporter = make_compression_reporter(hooks_);
    auto before = history_;
    
    CompressionConfig cc = load_compression_config(cfg_);
    history_ = compression_->compress(before, cc, client_, reporter.get());
    
    // Compute stats
    CompressionResult r = build_compression_result(before, history_);
    
    // Memory ops (not in CompressionPipeline)
    if (memory_store_ && ...) { ... }
    
    last_compression_ = r;
    return r;
}
```

4. `Agent::compress_now()` body must be ≤40 lines after extraction.

### Refactor Rules

- `CompressionObserver` is a pure port; keep it in `include/agent/compressor.h`.
- The default no-op implementations mean consumers that don't need observation pay no cost.
- Do NOT add `AgentHooks` to `CompressionPipeline` — keep it decoupled.
- The `CompressionReporter` adapter (implementing `CompressionObserver`) lives in `lib/agent.cpp` (anonymous namespace) and maps observer calls to `hooks_.on_status`.
- After refactor, `compress_now()` should not contain any loop-collapse, build-request, LLM-call, or parse logic — those are all inside `CompressionPipeline::compress()`.

### Verification

- [ ] `make clean && make && make test` passes
- [ ] `compress_now()` ≤40 lines
- [ ] Compression unit tests still pass (config, parse, apply)
- [ ] Manual: session with 100+ messages compresses via `/compress` in TUI
- [ ] Memory/skill ops still fire after compression (check `last_extraction_result()`)

---

## Task 5: Decouple test suite from TUI headers (M1)

| Field | Value |
|---|---|
| **ID** | `FIX-005` |
| **Severity** | 🟡 Medium |
| **Depends on** | None |
| **Blocks** | Nothing |
| **Estimated effort** | 3-4 hours |
| **Files touched** | `tests/run_tests.cpp`, `tui/textutil.h`, `tui/palette.h`, `tui/rich.h`, `tui/markdown.h` (or copies), `Makefile.in` |

### Problem

`tests/run_tests.cpp` includes `tui/textutil.h`, `tui/palette.h`, `tui/rich.h`, `tui/markdown.h`. This creates a dependency from the core test suite to the UI adapter.

### Target Architecture

**Option A (Preferred): Extract UI-agnostic utilities into the core library**
- `tui/textutil.cpp` → split: UTF-8/display utilities go to `lib/textutil.cpp` + `include/agent/textutil.h`
- `tui/rich.cpp` → keep in TUI (rendering-specific), but extract the pure data types
- `tui/palette.cpp` → colour pair allocation stays in TUI; command completer could move
- `tui/markdown.cpp` → the md4c rendering stays in TUI; any pure parsing goes to core
- After extraction, tests include from `include/agent/` instead of `tui/`.

**Option B (Simpler): Split test suite**
- `tests/run_tests.cpp` → split into `tests/core/` and `tests/tui/`
- Core tests link only `libagent.a`
- TUI tests link `libagent.a` + TUI objects + ncurses
- Both are run by `make test`

### Refactor Rules

- Option A is architecturally cleaner but requires moving code between directories.
- If moving code, ensure all SPDX headers are preserved.
- If splitting the test suite, add a `make test-tui` target and have `make test` run both.
- Do NOT add new `#include "tui/*"` to any file in `tests/` after this fix.

### Verification

- [ ] `make clean && make && make test` passes
- [ ] No `#include "tui/"` in `tests/run_tests.cpp`
- [ ] All previously tested TUI utilities still have test coverage (either in core tests or TUI tests)

---

## Task 6: Lighten dispatch.h include chain (M5)

| Field | Value |
|---|---|
| **ID** | `FIX-006` |
| **Severity** | 🟡 Medium |
| **Depends on** | None |
| **Estimated effort** | 30 minutes |
| **Files touched** | `include/agent/dispatch.h` |

### Problem

`include/agent/dispatch.h:10` `#include "agent/agent.h"` pulls in 6+ headers transitively. `dispatch.h` only needs: `json`, `Message`, `ToolResult`, `AgentHooks`, `ConversationLog`, `ToolRegistry`, `Config`.

### Fix

Replace the heavy `#include "agent/agent.h"` with direct, minimal includes:

```cpp
#include <string>
#include <vector>
#include <set>
#include "nlohmann/json.hpp"

namespace agent {
class ToolRegistry;
struct Config;
struct AgentHooks;
class ConversationLog;
struct Message;
// (forward declarations where possible)
```

Only include headers for types used by value or with inline methods.

### Refactor Rules

- Forward-declare everything possible. Only `#include` when the type is used by value, as a base class, or via inline method.
- If `Tool` pointer or reference is passed but the full definition isn't needed, forward-declare.
- Verify the change compiles transitively: `touch include/agent/dispatch.h && make` should succeed for all targets.

### Verification

- [ ] `make clean && make && make test` passes
- [ ] `touch include/agent/agent.h && make` does NOT recompile `dispatch.o` or any file that only included `dispatch.h`
- [ ] `dispatch.h` does not include `agent/agent.h` anymore

---

## Task 7: Add undocumented design patterns to AGENTS.md (L6)

| Field | Value |
|---|---|
| **ID** | `FIX-007` |
| **Severity** | 🔵 Low |
| **Depends on** | None |
| **Estimated effort** | 30 minutes |
| **Files touched** | `AGENTS.md` |

### Problem

5 design patterns in active use are not documented in AGENTS.md.

### Fix

Add to the "Design patterns in use" section:

```
- **Observer** — `AgentHooks` (via `std::function` callbacks) lets UIs observe the
  agent loop without the core knowing about them. More precise than "Template Method"
  since the hooks are set, not subclassed.
- **Command** — `ProcessStartTool` / `ProcessReadTool` / `ProcessStopTool` each
  encapsulate a background-process request as an object with a uniform `execute()`.
- **Protection Proxy** — `Workspace::confine()` guards filesystem access behind
  path-confinement checks, proxying the real filesystem.
- **Null Object** — `Agent::silent_hooks()` returns a no-op `AgentHooks` so internal
  confirmation exchanges never reach the scrollback, without null-checking at every call site.
- **Memento** — `compress_now()` snapshots `history_` before mutation and restores
  it on failure, capturing and rolling back state.
```

Also update the "Template Method / Hook" entry to clarify that while hooks use
`std::function` (more Observer-ish), the `Agent` class itself follows Template Method
for its internal steps (`chat_once`, `confirm_turn`).

### Verification

- [ ] `AGENTS.md` lists all 5 additional patterns with brief explanations
- [ ] Markdown renders cleanly (no broken bullets or code fences)

---

## Task 8: Add TDD/Red-Green policy to AGENTS.md (M2)

| Field | Value |
|---|---|
| **ID** | `FIX-008` |
| **Severity** | 🟡 Medium |
| **Depends on** | None |
| **Estimated effort** | 20 minutes |
| **Files touched** | `AGENTS.md` |

### Fix

Insert a new subsection under "Engineering principles":

```
### TDD / Red-Green-Refactor (mandatory)

- **Bug fixes** must start with a failing test that reproduces the bug. Only then
  is the production code changed (Red → Green). After the fix passes, the test is
  committed alongside the fix.
- **New features** must follow the same cycle: write a failing test that specifies
  the desired behaviour, implement until green, then refactor.
- **Coverage threshold**: new code paths must have ≥80% line coverage. The CI gate
  (`make test`) must pass before merge.
- **Hermetic tests**: mock the LLM by testing `LLMClient::parse_models` /
  `merge_server_info` directly; do not hit a live server in the unit suite.
- **Test granularity**: prefer many small `TEST(name)` blocks over a single large
  test function. Each test exercises one behaviour.
- **Test location**: behaviour changes go in `tests/run_tests.cpp`. New test files
  may be added for major modules (`tests/compressor_test.cpp`, `tests/agent_test.cpp`)
  — add them to `UNITTEST_OBJ` in `Makefile`.
```

### Verification

- [ ] `AGENTS.md` contains the TDD policy section
- [ ] Policy is consistent with existing test conventions

---

## Task 9: Add code review process and error handling conventions to AGENTS.md (M3, M4)

| Field | Value |
|---|---|
| **ID** | `FIX-009` |
| **Severity** | 🟡 Medium |
| **Depends on** | None |
| **Estimated effort** | 30 minutes |
| **Files touched** | `AGENTS.md` |

### Fix

Add to AGENTS.md under "Conventions":

```
### Code review (mandatory)

- Every PR must be reviewed by at least one other contributor before merge.
- Reviewers verify:
  - SOLID conformance (no new SRP violations, dependency direction correct)
  - Hexagonal boundaries intact (lib/ never #includes from tui/ or tools/)
  - Size limits respected (classes ≤200 lines, methods ≤10 lines)
  - Test coverage: the PR includes a failing test → fix → passing test sequence
  - No new clang-tidy or cppcheck warnings
  - No commented-out code, dead branches, or speculative generality
  - SPDX header present on new files

### Error handling conventions

- **Tools**: always return errors via `ToolResult{false, "", error_msg}`. Never throw
  from `Tool::execute()`. Catch unexpected exceptions and convert to `ToolResult`.
- **Library functions**: may throw `std::runtime_error` for truly exceptional conditions
  (transport failure, corrupt config). Do not throw for expected states (empty results,
  missing files) — return an error code, empty optional, or `ToolResult`.
- **Recoverable errors**: model errors (malformed JSON, HTTP 4xx/5xx) should be returned
  as assistant messages or error-flagged `ToolResult` so the LLM can self-recover.
- **Unrecoverable errors**: configuration corruption, curl init failure — throw at
  construction; the host (CLI/TUI) catches and reports.
- **Assertions**: use `assert()` only for invariants that should never fire in a correct
  program. Do not use asserts for input validation.
```

### Verification

- [ ] AGENTS.md contains the Code Review and Error Handling sections
- [ ] Existing codebase broadly matches the documented conventions (spot-check 5 sites)

---

## Task 10: Build system cleanup — separate tool objects from core (H4)

| Field | Value |
|---|---|
| **ID** | `FIX-010` |
| **Severity** | 🟠 High |
| **Depends on** | FIX-001 (cancel token in core removes tools.h dependency from core) |
| **Estimated effort** | 2-3 hours |
| **Files touched** | `Makefile.in`, potentially `configure`, `GNUmakefile` |

### Problem

`Makefile.in:55-62` lists `tools/*.o` inside `LIB_OBJS`, building tool adapters into `libagent.a`. The AGENTS.md describes tools as an adapter layer separate from the core.

### Target Architecture

Split into two archives:

```
libagent_core.a   — lib/*.o only (domain core)
libagent_tools.a  — tools/*.o, tools/search/*.o (adapter implementations)
```

Both binaries link both archives:
```
amber:     src/main.o + libagent_core.a + libagent_tools.a
amber-tui: tui/*.o + libagent_core.a + libagent_tools.a + md4c + ncurses
test:      tests/run_tests.o + libagent_core.a + libagent_tools.a
```

### Refactor Rules

- Keep the `lib` phony target concise: `lib: libagent_core.a libagent_tools.a`
- Do NOT change any source code — only the build system.
- If any core file transitively depends on a tool file (currently `http_transport.cpp` → `tools.h`), FIX-001 must be done first.
- If `register_default_tools` is in `lib/tools_default.cpp` and includes tool factories, it must be in `libagent_tools.a` or `libagent_core.a` — whichever makes the dependency graph acyclic. Likely `libagent_tools.a` since it calls `make_bash_tool`, `make_search_tool`, etc.
- Update `make install` to install both archives.
- Update `make clean` to remove both archives.

### Verification

- [ ] `make clean && make lib` produces both `.a` files
- [ ] `make cli` and `make tui` link successfully against both archives
- [ ] `make test` runs and passes
- [ ] No `.o` from `tools/` ends up in the wrong archive
- [ ] `make install` works (or is tested manually)

---

## Task 11: Clean up naive memory extraction heuristic (M6)

| Field | Value |
|---|---|
| **ID** | `FIX-011` |
| **Severity** | 🟡 Medium |
| **Depends on** | FIX-002 (must be done first — touches the same thread in chat_once) |
| **Estimated effort** | 2-3 hours |
| **Files touched** | `lib/agent.cpp`, `include/agent/experience.h` (maybe) |

### Problem

The async extraction in `chat_once` (lib/agent.cpp:140-161) uses arbitrary length heuristics (50 < content.size() < 5000) to classify tool results as memories. This captures noise (command output, config dumps) and misses genuine knowledge.

### Target Architecture

**Option A: Remove heuristic extraction entirely**
- The full LLM-based extraction in `compress_now()` is the correct path.
- The heuristic was a placeholder. Remove it.

**Option B: Quality-gated heuristic**
- Only extract when `memory_store_` exists AND the tool result has high "knowledge signal":
  - Tool name is `read` or `search` (not `bash` or `write`)
  - Content contains natural language (heuristic: ratio of alphabetic to non-alphabetic chars > 0.7)
  - Content is not a duplicate of existing memories (check via `MemoryStore` lookup)
- Extract the heuristic into a free function `bool is_knowledge_candidate(const ToolResult&)` in `agent_helpers.h`.

Prefer Option A unless the heuristic demonstrably provides value.

### Verification

- [ ] `make clean && make && make test` passes
- [ ] If Option A: `grep -n 'content.size()' lib/agent.cpp` does not return the heuristic lines
- [ ] If Option B: unit tests exist for `is_knowledge_candidate()`

---

## Task 12: Add noexcept correctness (L1)

| Field | Value |
|---|---|
| **ID** | `FIX-012` |
| **Severity** | 🔵 Low |
| **Depends on** | None (can be done incrementally with other tasks) |
| **Estimated effort** | 1 hour |
| **Files touched** | `include/agent/tool.h`, `include/agent/search_backend.h`, `include/agent/config.h`, `include/agent/tools.h`, others |

### Fix

Add `noexcept` to:

- `Tool::name()` — pure virtual, never throws
- `Tool::description()` — pure virtual, never throws
- `Tool::parameters_schema()` — pure virtual, never throws
- `Tool::requires_approval()` — default returns false, never throws
- `Tool::is_read_only()` — default returns false, never throws
- `Tool::summarize()` — never throws
- `SearchBackend::name()` — pure virtual, never throws
- `Config::api_url()` — simple string concatenation
- `Config::models_url()` — simple string concatenation
- All `make_*_tool()` factory functions
- `ToolRegistry::empty()` — trivial
- `Stats` struct: all fields are arithmetic types

Do NOT add `noexcept` to `Tool::execute()` — it may throw on some implementations (though the contract says it shouldn't; document that it's expected to be noexcept once all implementations comply).

### Refactor Rules

- When adding `noexcept` to a virtual function, the override must also be `noexcept`. Check all implementations.
- `noexcept` is part of the function's type; verify that function pointers or `std::function` assignments still compile.

### Verification

- [ ] `make clean && make && make test` passes
- [ ] No "missing noexcept" warnings from clang-tidy (check with `make lint`)
- [ ] All Tool implementations (BashTool, ReadTool, WriteTool, SearchTool, ProcessStartTool, ProcessReadTool, ProcessStopTool) have `noexcept` on their overrides of the marked methods

---

## Task 13: Extract POSIX opendir from SessionStore (L2)

| Field | Value |
|---|---|
| **ID** | `FIX-013` |
| **Severity** | 🔵 Low |
| **Depends on** | None |
| **Estimated effort** | 1-2 hours |
| **Files touched** | `lib/session.cpp`, possibly `include/agent/session.h` |

### Problem

`SessionStore::list()` (lib/session.cpp:206-228) uses `::opendir` / `::readdir` / `::closedir` directly alongside JSON. Cross-concern.

### Fix

Extract a `list_json_files` helper using `std::filesystem::directory_iterator`:

```cpp
namespace {
std::vector<std::string> list_json_files(const std::string& dir) {
    std::vector<std::string> files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".json") {
            std::string name = entry.path().filename().string();
            if (name != "index.json" && name != "workspace.json")
                files.push_back(name);
        }
    }
    return files;
}
}
```

Then use it in `list()`. Remove the `#include <dirent.h>`.

### Refactor Rules

- The existing behaviour (skipping `index.json` and `workspace.json`) must be preserved.
- `std::filesystem` is C++17 — already required by the project.
- Verify the `list()` still returns newest-updated first (the sort at the end is unchanged).

### Verification

- [ ] `make clean && make && make test` passes
- [ ] `grep -n 'opendir\|readdir\|closedir\|dirent' lib/session.cpp` returns nothing
- [ ] Unit tests for session listing pass (if any exist)

---

## Task 14: Make Workspace instance-based to fix test isolation (L4)

| Field | Value |
|---|---|
| **ID** | `FIX-014` |
| **Severity** | 🔵 Low |
| **Depends on** | None |
| **Estimated effort** | 2-3 hours |
| **Files touched** | `lib/workspace.cpp`, `include/agent/workspace.h`, all callers (tools, tests, main.cpp, tui) |

### Problem

`Workspace` is entirely static methods backed by a function-local static `std::string root`. Tests cannot independently set the root — they interfere with each other.

### Target Architecture

Option A (Minimal): Add a static `reset()` method for tests:

```cpp
static void reset();  // test only — clears cached root
```

Option B (Clean): Make `Workspace` instance-based:

```cpp
class Workspace {
public:
    explicit Workspace(const std::string& root = "");
    std::string root() const;
    std::string local_dir() const;
    bool confine(const std::string& path, std::string& resolved, std::string& error) const;
    void set_root(const std::string& path);
};
```

Then inject it into tools and Agent via Config or constructor. This is the architecturally correct solution but requires touching every tool.

Prefer Option A for now (minimal change, solves the test problem). Option B is the long-term target.

### Verification

- [ ] `make clean && make && make test` passes
- [ ] Tests that call `Workspace::set_root()` no longer leak state across test boundaries
- [ ] If Option A: document as `// test only` and note that Option B is the long-term target

---

## Dependency Graph

```
FIX-001  (cancel token in core)
   │
   ├── FIX-003  (decompose Agent::run) — optional dep, reduces conflicts
   ├── FIX-010  (build system split) — hard dep
   │
FIX-002  (detached thread)
   │
   ├── FIX-011  (memory heuristic) — hard dep (same code area)
   │
FIX-004  (compress_now dedup) — depends on FIX-003 (same file)
   │
FIX-005  (test/TUI decoupling) — independent
FIX-006  (dispatch.h includes) — independent
FIX-007  (document patterns) — independent
FIX-008  (TDD policy) — independent
FIX-009  (review + error conventions) — independent
FIX-012  (noexcept) — independent
FIX-013  (opendir → filesystem) — independent
FIX-014  (Workspace isolation) — independent
```

**Recommended execution order:**

1. FIX-001 (core blocker)
2. FIX-002 (correctness blocker)
3. FIX-010 (build cleanup, needs FIX-001)
4. FIX-003 + FIX-004 (decomposition, in sequence)
5. FIX-011 (heuristic cleanup, needs FIX-002)
6. FIX-005, FIX-006, FIX-012, FIX-013, FIX-014 (parallel friendly)
7. FIX-007, FIX-008, FIX-009 (documentation, any time)
