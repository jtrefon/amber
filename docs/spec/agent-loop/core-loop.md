## Spec: Agent Core Loop

### Purpose
The central ReAct loop that drives the agent: receive a user prompt, orchestrate
LLM calls and tool executions in a turn-take loop, and return a final answer.
Every higher-level feature (mode system, tool dispatch, compression, memory,
confirmation) is a plug-in to this loop.

### Ownership
- **Source files**: `lib/agent.cpp` (`Agent::run()`, `chat_once()`, `confirm_turn()`, `compress_now()`), `lib/agent_helpers.cpp` (`safe_chat_once`, `empty_turn_reply`, `fingerprint_tool_calls`), `lib/dispatch.cpp` (`dispatch_tool_calls`), `lib/tool_recovery.cpp` (`FailStreak`), `include/agent/agent.h`
- **Test files**: `tests/run_tests.cpp` — 6 agent-loop test blocks

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | `Agent::run(user_prompt: string) → string`. The `Agent` must be configured with `LLMClient`, `ToolRegistry`, `Config`, and optional `CompressionStrategy`, `CompressionGate`, `MemoryRetriever`, `ConversationLog`. |
| **Output** | Final reply string (model text after tool loops resolve, or fallback if empty). |
| **Error states** | LLM error → retry once then continue. Loop detection → hard stop with `[loop detected]`. Empty turn → fallback message. Approval denied → tool skipped (not abort). |
| **Invariants** | See below. |
| **Thread safety** | `run()` blocks the calling thread. The caller typically launches it on a dedicated `std::thread`. Hooks fire on the agent thread; UI queues them via mutex + event queue. |

### Invariants

1. `history_` always starts with a system message (seeded by `ensure_system_prompt()` before the first LLM call).
2. Every LLM response is pushed to `history_` before the next iteration.
3. Every tool result is pushed to `history_` as a `role: "tool"` message immediately after execution.
4. The loop never exceeds `cfg_.max_tool_iterations` (default 100).
5. `run()` always returns a non-empty string. If no valid reply exists, `empty_turn_reply()` synthesises one.
6. The loop state machine follows: `Idle → Waiting → Thinking/Streaming → Tooling → Idle` (or loop back to Waiting).
7. The confirmation turn ("Are you finished?") is silent — no tokens leak to hooks that the UI sees.
8. Cancellation via `cancel_token` is checked by the HTTP transport and tools but NOT directly by the loop (loop relies on LLM call aborting via transport cancellation).

---

### Scenarios

#### [CL-01] Simple text reply (no tools)

- **Given**: An LLM that returns plain text, no tool calls
- **Input**: `run("What is 2+2?")`
- **Expected**: Single LLM call. Reply found to have no tool calls. `confirm_turn()` fires — model says "done". Reply `"4"` returned. Exactly 1 LLM call + 1 confirmation call.
- **On failure**: Additional spurious LLM calls, or empty reply.

#### [CL-02] Single tool call, then reply

- **Given**: An LLM that calls `read` tool on first turn, then replies on second
- **Input**: `run("Read foo.txt and summarise")`
- **Expected**: Iteration 1: tool call → `dispatch_tool_calls()` → tool result pushed. Iteration 2: text reply → confirmation → accepted. Returned: final summary.
- **Invariant**: Each LLM call alternates between having `tool_calls` and not. No `tool_calls` in the confirmation round.

#### [CL-03] Multiple tool calls in one turn (parallel)

- **Given**: LLM requests `read(a.txt)` and `read(b.txt)` in the same response
- **Input**: `run("Read both files")`
- **Expected**: Both tool calls extracted from single LLM response. `dispatch_tool_calls()` launches both via `std::async`. Results collected in call order. Both tool-result messages pushed to `history_`. Loop continues.
- **On failure**: Sequential execution (not parallel), or results out of order in history.

#### [CL-04] Tool call with JSON parse error in arguments

- **Given**: LLM returns `tool_calls` where one call's `arguments` is not valid JSON
- **Input**: `{\"name\": \"read\", \"arguments\": \"{bad json}\"}`
- **Expected**: `chat_stream()` (llm.cpp:73-88) validates all tool call arguments. If ANY call has non-JSON arguments, ALL calls are discarded. The call proceeds as text-only.
- **Rationale**: Conservative — one bad call should not silently drop arguments.

#### [CL-05] Tool loop detection (3+ identical calls)

- **Given**: Tool loop detection enabled (`cfg_.detection_loop = true`)
- **Input**: LLM repeatedly calls `read(foo.txt)` with same args, 3+ turns in a row
- **Expected**: `dispatch_with_loop_detection()` fingerprints each call set. At count=3, hard stop. Returns `"[loop detected: the model called tool(s) 'read' 3 times with the same arguments]"`.
- **Recovery**: No further LLM calls. Loop breaks.
- **Regression guard**: `agent_stops_on_repeated_empty_arg_tool_call` test.

#### [CL-06] Text loop detection (5+ identical replies)

- **Given**: Text loop detection enabled
- **Input**: LLM returns same text content 5+ times consecutively
- **Expected**: At count=2: steer message injected ("You are repeating yourself..."). At count=5: hard stop with `"[loop detected: the model repeated itself 5 times]"`.
- **On recovery**: If steer works (model changes text at count=3 or 4), counter resets.

#### [CL-07] FailStreak recovery (3+ consecutive tool failures)

- **Given**: Tool returns `ok=false` 3+ turns in a row
- **Input**: LLM calls `read(nonexistent)` → fail; same call → fail again; same call → fail third time
- **Expected**: After 3 consecutive `ok=false` batches, `FailStreak` triggers `inject_tool_recovery_steer()`. A steer message is inserted into history suggesting the model verify paths before retrying. At streak=6 (2nd recovery attempt), hard stop.
- **On failure**: Infinite re-try loop without recovery steer.

#### [CL-08] LLM error — retry once

- **Given**: LLM client throws (network error, HTTP 5xx)
- **Input**: `run("hello")`
- **Expected**: `safe_chat_once()` catches the exception, returns `Message("[error during generation: ...]")`. `run()` detects the error prefix, retries once. If success: normal flow. If retry also fails: error message pushed to history, loop continues.
- **Rationale**: Compression destroys KV cache. Retry before compression to keep KV cache intact.

#### [CL-09] Confirmation accepts reply

- **Given**: Model produces text-only reply
- **Input**: Model says "The answer is 42."
- **Expected**: `confirm_turn()` injects "Are you finished?" + model says "done" → original reply returned.
- **On failure**: Confirmation loop (model refuses to confirm, keeps going) — prevented by `max_tool_iterations` bound.

#### [CL-10] Confirmation triggers more tool calls

- **Given**: Model produces text-only reply but when asked "Are you finished?", realises more info needed
- **Input**: After text reply, confirmation asks → model calls `search("...")`
- **Expected**: Confirmation tool calls dispatched. If at least one tool runs, returns `""` (continue main loop). If all tools denied, returns original candidate to prevent infinite loop.
- **Denied-tool guard**: Checks last tool result for `"status=denied"`.

#### [CL-11] Empty turn fallback

- **Given**: Loop exits with empty `final_reply` (max iterations, hard stop with no final text)
- **Input**: Any scenario that exhausts loop without producing usable output
- **Expected**: `finish_turn("")` calls `empty_turn_reply()`. If history has tool calls: `"[agent stopped: the model stopped producing usable output after tool calls; see the ERROR messages above]"`. If no tool calls: `"[agent stopped: the model produced no usable response]"`.
- **Invariant**: `run()` never returns empty string.

#### [CL-12] System prompt is idempotent

- **Given**: Multiple calls to `ensure_system_prompt()`
- **Input**: Two `run()` invocations on the same `Agent` instance
- **Expected**: System prompt inserted once at the start of `history_`. Second `run()` sees `history_[0].role == "system"` and skips insertion.
- **On failure**: Duplicate system message grows history unboundedly.

#### [CL-13] Memory retrieval injected per-turn

- **Given**: `MemoryRetriever` configured with relevant memories
- **Input**: `run("Tell me about project X")`
- **Expected**: Before each LLM call, `chat_once()` retrieves up to 500 tokens of relevant memories. A suffix is appended to the system message in the prompt copy (not stored history).
- **On failure**: Memories not retrieved, or retrieved into stored history (corrupting it).

#### [CL-14] Silent hooks during confirmation

- **Given**: Confirmation turn
- **Input**: `confirm_turn()` calls `chat_once(tools, display=false)`
- **Expected**: `silent_hooks()` returns an `AgentHooks` with only `on_state` (no-op). No `on_token`, `on_assistant`, `on_tool_call`, etc. fire during the confirmation exchange.
- **Rationale**: Prevents internal "Are you finished?" dialog from appearing in UI scrollback.

#### [CL-15] Cancellation during streaming

- **Given**: User presses Esc while agent is streaming
- **Input**: `cancel_token.request()` called
- **Expected**: HTTP transport's `cancel_check_cb()` polls the token and aborts the curl transfer. `chat_stream()` returns partial data. `safe_chat_once` may return a partial reply or error message. Loop continues with whatever was received.
- **On failure**: Agent hangs until timeout (60s default).

#### [CL-16] Max iterations reached

- **Given**: Model keeps calling tools without producing final reply
- **Input**: `run("explore this deeply")`, model calls tool → tool result → calls tool → ...
- **Expected**: At iteration `cfg_.max_tool_iterations` (default 100), loop exits. `empty_turn_reply()` returns fallback.
- **On failure**: Infinite loop hangs the agent.

#### [CL-17] Read mode denies write tools

- **Given**: `cfg_.mode = AgentMode::Read`
- **Input**: Model calls `write` tool
- **Expected**: `dispatch_tool_calls()` → approval gate checks `mode == Read && !tool.is_read_only()` → returns `ToolResult{ok=false, meta={{"denied", true}}}`. `format_tool_envelope` produces `status=denied`.
- **On failure**: Write tool executes in read mode.

#### [CL-18] Yolo mode auto-approves all tools

- **Given**: `cfg_.mode = AgentMode::Yolo`
- **Input**: Model calls `bash` tool
- **Expected**: Approval gate → Yolo mode → auto-approve without callback. Tool executes immediately.
- **On failure**: Approval dialog shown (blocking) or tool denied.

---

### Cross-references

- **Depends on**: `agent-loop/tool-dispatch.md`, `agent-loop/mode-system.md`, `agent-loop/error-recovery.md`, `llm-client/streaming.md`, `compression/compression-pipeline.md`, `memory/extraction.md`
- **Depended on by**: `tui/event-loop.md`, `docs/spec/INDEX.md` (agent-loop category)
- **Test coverage**: `tests/run_tests.cpp` — `agent_retains_history_across_turns`, `agent_denies_gated_tool_without_handler`, `agent_stops_on_repeated_empty_arg_tool_call`, `agent_text_only_reply`, `agent_loop_behavioral_spec`

### Known gaps

1. **No direct `run()` tests with mock LLM** — Agent loop tests mock only the SSE server for `chat_stream`. `confirm_turn()`, `error retry`, and `empty_turn_fallback` are not directly tested.
2. **`session_approved_` never cleared** — Tools granted `AllowSession` stay approved for the Agent's lifetime. Only `reset()` clears it.
3. **No `history_.pop_back()` rollback** — If `confirm_turn()` decides to continue, the "Are you finished?" exchange stays in history by design (context), but there's no mechanism to undo erroneous pushes.
4. **CancellationToken not polled by loop** — The loop itself does not check `cancel_token.is_requested()`. It relies on the LLM call aborting via transport polling. If the model takes too long to respond without streaming, cancellation may be delayed.
5. **Compression in `chat_once()` operates on copy only** — Automatic compression is non-destructive (compresses prompt copy, not stored history). Only `/compress` replaces `history_`.
