## Spec: Agent Core Loop

### Purpose
The central ReAct loop that drives the agent: receive a user prompt, orchestrate
LLM calls and tool executions in a turn-take loop, and return a final answer.
Every higher-level feature (mode system, tool dispatch, compression, memory,
detached work) is a plug-in to this loop.

**Turn contract (2026-10-18):** the model decides when a turn ends. The loop
continues while the model requests tools and stops on a reply that carries none —
that reply is the answer. The harness never asks for another round and never
invents a turn of its own; what it has to report (a retry, a repair, a budget, a
stop reason) it says as a `[harness]` notice, and the model keeps the next word.

### Ownership
- **Source files**: `lib/agent.cpp` (`Agent::run()`, `chat_once()`, `advance_turn()`, `inject_harness_note()`, `compress_now()`), `lib/agent_helpers.cpp` (`chat_with_retry`, `empty_turn_reply`, `fingerprint_tool_calls`), `lib/dispatch.cpp` (`dispatch_tool_calls`), `lib/tool_recovery.cpp` (`FailStreak`), `include/agent/agent.h`
- **Test files**: `tests/agent_loop_test.cpp` (hermetic FakeLLMClient scenarios [AL-xx]), `tests/agent_events_test.cpp`

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | `Agent::run(user_prompt: string) → string`. The `Agent` must be configured with `LLMClient`, `ToolRegistry`, `Config`, and optional `CompressionStrategy`, `CompressionGate`, `MemoryRetriever`, `ConversationLog`. |
| **Output** | Final reply string (model text after tool loops resolve, or fallback if empty). |
| **Error states** | LLM error → up to 3 attempts with backoff, then a `[harness]` notice naming the attempts (the model decides the next move). Loop detection → hard stop with `[loop detected]`. Empty reply after tools → one nudge, then the bound stops the turn. Approval denied → tool skipped (not abort). |
| **Invariants** | See below. |
| **Thread safety** | `run()` blocks the calling thread. The caller typically launches it on a dedicated `std::thread`. Hooks fire on the agent thread; UI queues them via mutex + event queue. |

### Invariants

1. `history_` always starts with a system message (seeded by `ensure_system_prompt()` before the first LLM call).
2. Every LLM response is pushed to `history_` before the next iteration.
3. Every tool result is pushed to `history_` as a `role: "tool"` message immediately after execution.
4. The loop never exceeds `cfg_.max_tool_iterations` (default 100).
5. `run()` always returns a non-empty string. If no valid reply exists, `empty_turn_reply()` synthesises one.
6. The loop state machine follows: `Idle → Waiting → Thinking/Streaming → Tooling → Idle` (or loop back to Waiting).
7. Harness notices reach the model in place: a `[harness]` message is pushed with
   `Message::harness` set, so the loop knows it is not the model's own reply and
   the model knows it is the environment speaking.
8. Cancellation via `cancel_token` is checked by the HTTP transport and tools but NOT directly by the loop (loop relies on LLM call aborting via transport cancellation).

---

### Scenarios

#### [CL-01] Simple text reply (no tools)

- **Given**: An LLM that returns plain text, no tool calls
- **Input**: `run("What is 2+2?")`
- **Expected**: One LLM call. A reply with no tool calls **is** the answer: `"4"` is returned and nothing is generated after it (no injected user turn, no probe).
- **On failure**: Additional spurious LLM calls, or empty reply.

#### [CL-02] Single tool call, then reply

- **Given**: An LLM that calls `read` tool on first turn, then replies on second
- **Input**: `run("Read foo.txt and summarise")`
- **Expected**: Iteration 1: tool call → `dispatch_tool_calls()` → tool result pushed. Iteration 2: text reply → the turn ends with that summary.
- **Invariant**: Each LLM call alternates between having `tool_calls` and not; a reply without them ends the turn.

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
- **Expected**: At count=2: a `[harness]` steer is injected. At count=5: hard stop with `"[loop detected: the model repeated itself and did not recover. Please rephrase your request.]"`.
- **On recovery**: If the steer works (model changes text at count=3 or 4), counter resets.

#### [CL-07] FailStreak recovery (3+ consecutive tool failures)

- **Given**: Tool returns `ok=false` 3+ turns in a row
- **Input**: LLM calls `read(nonexistent)` → fail; same call → fail again; same call → fail third time
- **Expected**: After 3 consecutive `ok=false` batches, `FailStreak` triggers `inject_tool_recovery_steer()`. A steer message is inserted into history suggesting the model verify paths before retrying. At streak=6 (2nd recovery attempt), hard stop.
- **On failure**: Infinite re-try loop without recovery steer.

#### [CL-08] LLM error — bounded retry, then reported

- **Given**: LLM client throws (network error, HTTP 5xx)
- **Input**: `run("hello")`
- **Expected**: `chat_with_retry()` retries up to 3 attempts with 1s→2s backoff (each attempt silent — a recovered hiccup is not the model's business). On exhaustion the model receives a `[harness]` notice naming the attempt count; the loop continues so the next reply can react.
- **Rationale**: Compression destroys KV cache. Retry before compression to keep KV cache intact.
- **Test**: `agent_loop_retry_then_success`, `agent_loop_recovered_outage_is_silent`, `agent_loop_retries_exhausted`.

#### [CL-09] A harness notice hands the model the next word

- **Given**: A generation that fails every attempt (503 storm), so `chat_with_retry` returns a notice instead of a reply
- **Input**: `run("...")`
- **Expected**: One `role="user"`, `Message::harness = true` message naming the stage and the attempt count is pushed; the loop continues, so the model's next reply can react to it (adjust, try another approach, or tell the user what is blocked). The notice is never `role="assistant"` and never ends the turn by itself.
- **Bound**: Three consecutive no-answer rounds (empty replies or unanswered notices) stop the turn with `empty_turn_reply()`.
- **Tests**: `agent_loop_retries_exhausted`, `agent_loop_non_retryable_fails_fast`, `agent_unrecoverable_request_reports_instead_of_throwing`.

#### [CL-10] A repeated narration while tools keep running trips the opt-in text guard

- **Given**: Text loop detection enabled (`cfg_.detection_loop = true`) and a model that narrates the same line while calling different tools
- **Input**: Six iterations of the same text with a different tool call each
- **Expected**: At repeat 2 a `[harness]` steer is injected; at repeat 5 the turn hard-stops with `[loop detected: …]`.
- **Test**: `agent_loop_text_loop_detection`.

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

#### [CL-14] Harness notices are typed, not borrowed voices

- **Given**: Any notice the harness must deliver (retry summary, repair report, stop reason)
- **Input**: A failed request, a loop bound firing, a repair applied mid-request
- **Expected**: The notice is a `role="user"` message with `Message::harness = true` and the `[harness]` prefix. It reaches the model in place (the OpenAI dialect hoists system messages to the head of the request, which would lose that place) and the loop never mistakes it for the model's own reply.
- **Invariant**: No harness text is ever pushed as `role="assistant"`.

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

1. **No direct `run()` tests with mock LLM** — closed: `tests/agent_loop_test.cpp` drives the real `Agent` and `Context` with a scripted `FakeLLMClient` ([AL-01]..[AL-12], plus the turn-contract and notice scenarios).
2. **`session_approved_` never cleared** — Tools granted `AllowSession` stay approved for the Agent's lifetime. Only `reset()` clears it.
3. **No `history_.pop_back()` rollback** — a turn's messages stay in history once sealed (the context is an immutable stack); the compression rebuild (`clear()` + `push()`) is the only rewrite.
4. **CancellationToken not polled by loop** — The loop itself does not check `cancel_token.is_requested()`. It relies on the LLM call aborting via transport polling. If the model takes too long to respond without streaming, cancellation may be delayed.
5. **Compression in `chat_once()` operates on copy only** — Automatic compression is non-destructive (compresses prompt copy, not stored history). Only `/compress` replaces `history_`.
