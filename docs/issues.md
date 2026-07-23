# amber — Current Issues Register

- **Status:** Active (in review)
- **Last updated:** 2026-07-23
- **Owner:** Jacek Trefon
- **Tolerance:** Zero technical debt — every issue must be refactored, not patched

---

## Classification Key

| Severity | Colour | Meaning |
|----------|--------|---------|
| 🔴 Critical | Red | Blocks correctness or safety. Must fix before next release. |
| 🟠 High | Orange | Violates documented architecture or SOLID. Must fix before claiming 0-debt. |
| 🟡 Medium | Yellow | Architectural boundary violation or missing policy. Must fix per iteration plan. |
| 🔵 Low | Blue | Style, completeness, or documentation gap. Fix opportunistically. |

---

## 🔴 Critical

### C1 — Detached thread use-after-free in chat_once extraction

| Field | Value |
|---|---|
| **File** | `lib/agent.cpp:140-161` |
| **Pattern** | `std::thread([this]{...}).detach()` |
| **Root cause** | `Agent::chat_once()` fires async experience extraction on a detached thread capturing `this`. If the `Agent` is destroyed before the thread completes, all member accesses are use-after-free. |
| **Why it exists** | The extraction is fire-and-forget for UX (don't block the reply), but ownership was not tracked. |
| **Impact** | Non-deterministic crashes on agent shutdown, especially in the TUI where `Tui` is destroyed on `/quit`. |
| **Scope** | Single method, but affects the entire Agent lifetime. |
| **Fix class** | **Refactor** — replace `detach()` with a tracked future or a work queue with cancellation. |

### C2 — HTTP transport depends on tool-cancel globals (hexagonal boundary violation)

| Field | Value |
|---|---|
| **File** | `lib/http_transport.cpp:7` ← `tools/bash_tool.cpp:27-33` |
| **Pattern** | Core `#include "agent/tools.h"` for `is_tool_cancel_requested()` |
| **Root cause** | The cancel-check callback (`cancel_check_cb`) calls a free function whose atomics live in a tool adapter (`tools/bash_tool.cpp`). The core library depends on an adapter layer. |
| **Why it exists** | The cancel mechanism was added post-hoc to unblock streaming, and the atomics were colocated with the only consumer (bash tool) for convenience. |
| **Impact** | If tools are ever conditionally compiled or replaced, the core will not link. Also prevents multiple Agent instances from having independent cancel state. |
| **Scope** | Two files, but architectural — touches `http_transport.cpp`, `http_transport.h`, `tools.h`, `bash_tool.cpp`. |
| **Fix class** | **Refactor** — move cancel atomics into the core under a dedicated port (`CancellationToken`), inject at the boundary. |

---

## 🟠 High

### H1 — Agent::run() violates SRP and size limit

| Field | Value |
|---|---|
| **File** | `lib/agent.cpp:383-538` |
| **Metric** | ~155 lines. AGENTS.md mandates <10 lines/method. |
| **Responsibilities** | 1) Prompt seeding, 2) Logging, 3) Conversation loop, 4) Tool-loop detection (identical calls), 5) Text-loop detection (repeated replies), 6) FailStreak + recovery steer, 7) Confirmation handoff (`confirm_turn`), 8) Empty-reply fallback. |
| **Why it exists** | The run loop grew organically as detection heuristics and recovery mechanisms were added. Previous decomposition of `agent.cpp` (473→200 lines) missed the run loop body itself. |
| **Impact** | Every new detection feature or recovery heuristic inflates this method further. Untestable at the unit level (too many paths). Breaches documented contract. |
| **Scope** | Single method; refactoring does not change public API. |
| **Fix class** | **Refactor** — extract each responsibility into a named private method. The loop body should become a sequence of calls, readable at the 5-line level. |

### H2 — Agent::compress_now() violates SRP and size limit

| Field | Value |
|---|---|
| **File** | `lib/agent.cpp:178-337` |
| **Metric** | ~160 lines. 6+ sequential steps. |
| **Responsibilities** | 1) Snapshot state, 2) Collapse loops, 3) Build + append compression request, 4) Call LLM, 5) Parse response, 6) Build per-turn tags, 7) Apply classification, 8) Apply memory/skill ops, 9) Status reporting. |
| **Why it exists** | The compression pipeline was added as a single monolithic method for simplicity; later steps (memory ops, status) were appended rather than extracted. |
| **Impact** | Unreadable, untestable in isolation. Duplicates logic from `CompressionPipeline::compress()` in `compressor.cpp` (the same steps exist in both places). |
| **Scope** | Single method. The `CompressionPipeline` class in `compressor.cpp` already implements a similar pipeline — this is a DRY violation. |
| **Fix class** | **Refactor** — reuse `CompressionPipeline::compress()` instead of duplicating the pipeline in `Agent`. Extract status reporting into a callback or decorator. |

### H3 — Tool cancel is module-level global state

| Field | Value |
|---|---|
| **File** | `tools/bash_tool.cpp:27-33`, declared in `include/agent/tools.h` |
| **Pattern** | `static std::atomic<bool> g_tool_cancel` at file scope |
| **Root cause** | `request_tool_cancel()` / `is_tool_cancel_requested()` / `clear_tool_cancel()` operate on a global flag. There is no instance-scoped cancellation. |
| **Why it exists** | Quick way to make the HTTP transport poll a cancel flag without invasive API changes. |
| **Impact** | 1) Two `Agent` instances interfere with each other's cancellation. 2) Race between `clear_tool_cancel()` and a new request starting. 3) Cannot test cancellation without global state reset. |
| **Scope** | Affects `tools.h`, `bash_tool.cpp`, `http_transport.cpp`, and any tool that checks cancellation. |
| **Fix class** | **Refactor** — replace globals with an injected `CancellationToken` (shared `std::atomic<bool>` owned by the host). |

### H4 — Tools compiled into libagent.a (build-layer boundary blur)

| Field | Value |
|---|---|
| **File** | `Makefile.in:55-62` |
| **Pattern** | `tools/*.o` listed in `LIB_OBJS`, linked into `libagent.a` |
| **Root cause** | The makefile does not distinguish between core objects (`lib/`) and adapter objects (`tools/`). |
| **Why it exists** | Simpler build: one static library, both CLI and TUI link it. The AGENTS.md describes tools as an adapter layer but the build artefact disagrees. |
| **Impact** | Cannot conditionally omit tools (e.g. no bash in CI). Every tool change triggers full library relink. New tools require editing `Makefile.in` in two places. |
| **Scope** | Build system only. No source change. |
| **Fix class** | **Refactor** — split `libagent.a` into `libagent_core.a` (lib/) + `libagent_tools.a` (tools/), or build tools as a separate archive that both binaries link. |

---

## 🟡 Medium

### M1 — Tests include TUI headers (boundary violation)

| Field | Value |
|---|---|
| **File** | `tests/run_tests.cpp:12-15` |
| **Includes** | `tui/textutil.h`, `tui/palette.h`, `tui/rich.h`, `tui/markdown.h` |
| **Root cause** | TUI utilities (text wrapping, colour rendering) are only implemented in `tui/` and are tested from the core test suite. |
| **Why it exists** | These utilities were originally in `tui/` but need tests; rather than duplicate or extract them, the test file includes them directly. |
| **Impact** | TUI changes break core tests. The hexagonal boundary is pierced: the test suite (which should test core + adapters independently) cross-contaminates layers. |
| **Scope** | Test suite restructuring. No production code change. |
| **Fix class** | **Refactor** — extract UI-agnostic utilities (textutil, rich, palette) into the core library, or split the test suite into `tests/core/` and `tests/tui/`. |

### M2 — No TDD/Red-Green policy in AGENTS.md

| Field | Value |
|---|---|
| **File** | `AGENTS.md` |
| **Gap** | Zero mention of test-first workflow |
| **Why it exists** | The engineering principles section was written before the TDD policy was established. |
| **Impact** | Developers may write code before tests. Bug fixes may not be preceded by a failing test. |
| **Scope** | Documentation only. |
| **Fix class** | **Add** — Insert a "TDD / Red-Green-Refactor" section in AGENTS.md. |

### M3 — No code review process in AGENTS.md

| Field | Value |
|---|---|
| **File** | `AGENTS.md` |
| **Gap** | No documented review requirements |
| **Impact** | Inconsistent review depth. Architectural regressions may pass review. |
| **Scope** | Documentation only. |
| **Fix class** | **Add** — Document review checklist and process. |

### M4 — No error handling convention in AGENTS.md

| Field | Value |
|---|---|
| **File** | `AGENTS.md` |
| **Gap** | No guidance on throw vs ToolResult vs error code |
| **Impact** | Inconsistent error patterns across the codebase. Some errors throw, some return `ToolResult{false,...}`, some silently swallow. |
| **Scope** | Documentation + minor code audit. |
| **Fix class** | **Add + Audit** — Document the convention, then audit 5-10 error sites for consistency. |

### M5 — Heavy include chain in dispatch.h

| Field | Value |
|---|---|
| **File** | `include/agent/dispatch.h:10` |
| **Detail** | `#include "agent/agent.h"` pulls in `config.h`, `registry.h`, `llm.h`, `conversation_log.h`, `compressor.h`, `experience.h` |
| **Why it exists** | `dispatch.h` needs `json`, `Message`, `AgentHooks`, `ToolResult` — but these are all transitively available through the heavy `agent.h` umbrella. |
| **Impact** | Recompilation of `dispatch.cpp` and all its consumers when any unrelated header changes. ~500ms added to incremental builds. |
| **Scope** | Single header. |
| **Fix class** | **Refactor** — forward-declare what can be forwarded, include only what is directly needed (`nlohmann/json.hpp`, `string`, `vector`). |

### M6 — Naive memory extraction heuristic in chat_once

| Field | Value |
|---|---|
| **File** | `lib/agent.cpp:140-161` |
| **Detail** | Async extraction uses length heuristics (50 < size < 5000) to pick memories from tool results |
| **Why it exists** | Full LLM-based extraction (in `compress_now()`) is too slow for every turn. The heuristic was added as a lightweight approximation. |
| **Impact** | Low-quality memories: arbitrary length thresholds capture command output and configuration noise, not genuine knowledge. |
| **Scope** | Can be improved without API change. |
| **Fix class** | **Refactor** — either remove the heuristic extraction or make it opt-in with a quality gate. |

---

## 🔵 Low

### L1 — Missing noexcept on accessors and pure functions

| Where | Examples |
|---|---|
| `Tool::name()` | Should be `noexcept` |
| `Tool::is_read_only()` | Should be `noexcept` |
| `Tool::requires_approval()` | Should be `noexcept` |
| `SearchBackend::name()` | Should be `noexcept` |
| `Config::api_url()` | Should be `noexcept` |
| Various getters | ~20 methods that never throw |

**Scope:** ~20 trivial sites. Fix opportunistically alongside other refactors.

### L2 — SessionStore::list() uses POSIX opendir directly

| File | `lib/session.cpp:206-228` |
|---|---|
| **Detail** | `::opendir` / `::readdir` / `::closedir` for directory scanning. Self-acknowledged in AGENTS.md. |
| **Fix class** | Extract an `fs::list_json_files(dir)` helper using `std::filesystem::directory_iterator`. |

### L3 — Config is a concrete struct (not abstracted)

| File | `include/agent/config.h:21` |
|---|---|
| **Detail** | Config is a plain struct. No interface means no alternative implementations (env-file, config-server, DB-backed). Acceptable for current scope but precludes extension without modification (OCP violation). |
| **Fix class** | Extract a `ConfigSource` interface when a second source type is needed. Not a priority now. |

### L4 — Workspace uses function-local static for root

| File | `lib/workspace.cpp:15-18` |
|---|---|
| **Detail** | `std::string& root_storage() { static std::string root; return root; }` |
| **Impact** | Tests must be sequenced carefully. `set_root()` in one test leaks into the next. |
| **Fix class** | Make Workspace instance-based (instead of all-static) or add test-only reset. |

### L5 — Custom test framework lacks fixtures, matchers, parameterisation

| File | `tests/test_util.h` (93 lines) |
|---|---|
| **Detail** | Minimal harness: `TEST(name)`, `ASSERT(cond)`, `ASSERT_EQ(a,b)`. No `EXPECT_THROW`, `CONTAINS`, setup/teardown, or test filtering. |
| **Fix class** | Gradual enhancement. Acceptable for current project size. |

### L6 — 5 undocumented design patterns in active use

| Pattern | Location | Missing from AGENTS.md |
|---|---|---|
| Observer | `AgentHooks` via `std::function` | Listed as "Template Method / Hook" — Observer is more precise |
| Command | `ProcessStartTool` / `ReadTool` / `StopTool` | Not listed |
| Protection Proxy | `Workspace::confine()` | Not listed |
| Null Object | `Agent::silent_hooks()` | Not listed |
| Memento | `compress_now()` history snapshot | Not listed |

**Fix class:** Update AGENTS.md design patterns section.

---

## Superseded / Resolved

These issues were addressed in prior refactors (verified against current code):

- [x] `lib/llm.cpp` (511→84 lines) — split into request_builder, sse_parser, http_transport, model_probe, debug_log
- [x] `lib/agent.cpp` (473→200 lines) — confirmation loop and tool-dispatch extracted
- [x] `tui/tui.cpp` (1245→171 lines) — god-class split
- [x] `tui/widgets.cpp` (333 lines) — split into dialog, form_edit, info_dialog, menu_select
- [x] `tools/search/semantic_backend.cpp` (227→126 lines) — indexing helpers extracted
- [x] `tools/bash_tool.cpp` (191→196 lines) — execute() decomposed into run_with_timeout + drain_output
