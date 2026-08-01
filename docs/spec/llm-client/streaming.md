## Spec: LLM Streaming (SSE)

### Purpose
Stream LLM responses token-by-token over SSE (Server-Sent Events) so the TUI
can display text incrementally. The stream parser must handle three vendor-
specific behaviours: dedicated `reasoning_content` fields, inline `<think>` tags,
and fragmented tool-call arguments that arrive over multiple SSE events.

### Ownership
- **Source files**: `lib/llm.cpp` (`chat_stream()`), `lib/sse_parser.cpp` (`StreamParser`, `dispatch_event_impl`, `segment_think_impl`, `accumulate_arguments`, `finalize_impl`), `include/agent/sse_parser.h`, `include/agent/llm.h` (`StreamChunk`, `Message`)
- **Test files**: `tests/run_tests.cpp` — 4 SSE parser tests (lines 843–1003)

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | `Message` history + tool schemas + per-chunk callback `std::function<void(StreamChunk)>` |
| **Output** | Accumulated `Message` with `content`, `reasoning`, `tool_calls`. Hooks fire per-chunk: `on_token`, `on_reasoning`, `on_state`. |
| **Error states** | Connection drop → exception caught by `safe_chat_once()`. HTTP 4xx/5xx → throw with body excerpt. Malformed SSE → silently dropped. |
| **Invariants** | See below. |
| **Thread safety** | SSE write callback fires on libcurl thread. `on_chunk` callback is user-provided; the caller (`chat_once`) must handle thread safety. |

### Invariants

1. Every SSE event with `data: ` prefix is valid JSON, or silently dropped.
2. `[DONE]` marker terminates the stream exactly once (idempotent `finalize`).
3. Every `StreamChunk` with `delta` or `reasoning` fires the `on_chunk` callback exactly once per event.
4. The terminal chunk (`done = true`) is always emitted, even on empty streams.
5. Tool call `arguments` field is always a JSON string in the output `Message` (OpenAI wire format).
6. `<think>` tag boundaries that straddle SSE fragments are correctly detected.
7. If ANY tool call in the batch has non-JSON `arguments`, ALL tool calls are discarded.
8. `chunk.delta` and `chunk.reasoning` are never both non-empty in the same chunk.

---

### Scenarios

#### [ST-01] Stream text tokens

- **Given**: A single-chunk SSE stream with text content
- **Input**: `data: {"choices":[{"delta":{"content":"Hello"}}]}\n\ndata: [DONE]`
- **Expected**: `on_chunk` fires once with `chunk.delta = "Hello"`. Terminal chunk fires with `done = true`. Final `out.content = "Hello"`.
- **On failure**: Chunks dropped, or duplicate terminal chunk.

#### [ST-02] Stream multiple text fragments

- **Given**: Multi-chunk SSE stream
- **Input**: `data: ...{"delta":{"content":"Hello "}}` then `data: ...{"delta":{"content":"world"}}`
- **Expected**: Two `on_chunk` callbacks. `out.content = "Hello world"`.
- **On failure**: Missing content, wrong concatenation.

#### [ST-03] Dedicated reasoning field (`reasoning_content`)

- **Given**: SSE events with `reasoning_content` field
- **Input**: `data: ...{"delta":{"reasoning_content":"Let me think"}}`
- **Expected**: `chunk.reasoning = "Let me think"`. `on_reasoning` hook fires. `on_token` does NOT fire for this chunk. `out.reasoning = "Let me think"`.
- **Vendor compatibility**: Both `reasoning_content` and `reasoning` keys are checked.
- **Regression guard**: `llm_streaming_reasoning_content_field` test.

#### [ST-04] Inline `<think>` tag segmentation

- **Given**: Content with `<think>` tags spanning SSE fragments
- **Input**: `data: ...{"delta":{"content":"<thi"}}` then `data: ...{"delta":{"content":"nk>plan</think>Hello"}}`
- **Expected**: First chunk: `chunk.reasoning` does NOT fire (partial tag). Second chunk: `chunk.reasoning = "plan"`, `chunk.delta = "Hello"`. Final `out.reasoning = "plan"`, `out.content = "Hello"`.
- **Boundary guard**: 6-char `pending` buffer (`<think>` = 7 chars, `<\nthink>` = 8 chars) catches tag straddling.
- **Regression guard**: `llm_streaming_inline_think_segmentation` test.

#### [ST-05] Tool call — single-fragment string arguments

- **Given**: Tool call with complete arguments in one SSE event
- **Input**: `data: ...{"delta":{"tool_calls":[{"index":0,"id":"call_1","function":{"name":"read","arguments":"{\"path\":\"foo.txt\"}"}}]}}`
- **Expected**: `out.tool_calls[0].function.arguments = '{"path":"foo.txt"}'`. Arguments string is valid JSON.
- **On failure**: Arguments parse as discarded (non-JSON in string).

#### [ST-06] Tool call — multi-fragment string arguments

- **Given**: Arguments split across two SSE events
- **Input**: First: `"arguments":"{\\"path\\":"` — Second: `"\\"foo.txt\\"}"`
- **Expected**: String concatenation produces `'{"path":"foo.txt"}'`. Valid JSON.
- **Regression guard**: `llm_streaming_merges_tool_call_fragments` test.

#### [ST-07] Tool call — object-typed arguments merged

- **Given**: First delta sends object `{"pattern":"ncurses"}`, second delta sends string `"|curses"`
- **Input**: First fragment object → second fragment string
- **Expected**: `accumulate_arguments()` detects `cur_obj.is_object() && piece_obj.is_discarded()`, performs string concatenation. Final result: `'{"pattern":"ncurses"}'` plus `"|curses"` appended.
- **Regression guard**: `llm_streaming_tool_call_object_arguments_preserved` test.

#### [ST-08] Tool call — object-to-object merge

- **Given**: First delta sends `{"pattern":"ncurses"}`, second sends `{"limit":"10"}`
- **Input**: Both fragments parse as JSON objects
- **Expected**: In-memory JSON object is merged key-by-key. Final string: `'{"pattern":"ncurses","limit":"10"}'`.
- **On failure**: Only one key survives, or string concatenation produces double JSON.

#### [ST-09] Non-JSON tool call arguments discarded

- **Given**: Stream completes with one call having non-JSON arguments
- **Input**: `"arguments": "{bad json}`
- **Expected**: `chat_stream()` validates all calls post-stream. Non-JSON detected → ALL tool calls discarded (`out.tool_calls = null`). Message proceeds as text-only.
- **Rationale**: Non-JSON arguments in history poison subsequent requests.
- **On failure**: Malformed arguments reach the tool.

#### [ST-10] Usage chunk (final stats)

- **Given**: Stream ends with a usage-only chunk (empty choices)
- **Input**: `data: {"usage":{"prompt_tokens":50,"completion_tokens":100},"choices":[]}`
- **Expected**: `SseState.prompt_tokens = 50`, `SseState.completion_tokens = 100`. No `on_chunk` fires. `stream_completion()` populates `Stats` from parser.
- **Regression guard**: `llm_streaming_captures_usage_stats` test.

#### [ST-11] Malformed SSE data silently dropped

- **Given**: A garbled `data:` line in the stream
- **Input**: `data: not json`
- **Expected**: `dispatch_event_impl()` → `json::parse` returns discarded. Function returns silently. Stream continues to next event.

#### [ST-12] Empty content fields

- **Given**: SSE event with no `delta` or empty `content`
- **Input**: `data: {"choices":[{"delta":{}}]}`
- **Expected**: No `on_chunk` callback fired. Stream continues.

#### [ST-13] Connection drop mid-stream

- **Given**: Server closes connection mid-response
- **Input**: Partial SSE stream, then `CURLE_RECV_ERROR`
- **Expected**: `curl_exec()` throws. `safe_chat_once()` catches, returns error message. Agent retries once. The partial `out` message (with whatever was received) is lost — no partial push to history.
- **On failure**: Partial content pushed to history, corrupting conversation.

#### [ST-14] Cancellation during streaming

- **Given**: `cfg.cancel_token.request()` called while stream in progress
- **Input**: `cancel_check_cb()` returns 1 → `CURLE_ABORTED_BY_CALLBACK`
- **Expected**: `curl_exec()` throws. `safe_chat_once()` catches. Loop continues with error message.
- **On failure**: Transfer continues to completion.

#### [ST-15] Double-finalize guard

- **Given**: `[DONE]` event received AND `stream_completion()` calls `parser.finalize()` after transfer
- **Input**: Normal stream completion
- **Expected**: `finalize_impl()` guard `if (st.finished) return` prevents second terminal chunk. `on_chunk({done: true})` fires exactly once.
- **On failure**: Two terminal chunks, `on_chunk` called twice with `done`.

#### [ST-16] Streaming with no tools (`tools = []`)

- **Given**: No tool schemas, text-only response
- **Input**: Standard text SSE stream
- **Expected**: `chat_stream` works identically to with-tools case. Tool call accumulation is skipped (no `tool_calls` in delta). `Message.tool_calls` remains null.

---

### Cross-references

- **Depends on**: `llm-client/http-transport.md` (curl transport), `agent-loop/core-loop.md` (hook wiring)
- **Depended on by**: `display/markdown-parser.md` (stream preview rendering), `tui/event-loop.md` (token hook → live render)
- **Test coverage**: `tests/run_tests.cpp`: `llm_streaming_tool_call_object_arguments_preserved`, `llm_streaming_merges_tool_call_fragments`, `llm_streaming_inline_think_segmentation`, `llm_streaming_reasoning_content_field`, `llm_streaming_captures_usage_stats`

### Known gaps

1. **No test for `accumulate_arguments` with all fragment-type combinations** — Only object-first-then-string is tested. String-first-then-object and object-to-object merge are untested.
2. **No test for connection drop mid-stream** — No mock sends partial SSE then closes.
3. **No test for cancel during streaming** — No concurrent cancel_token.request() while stream is active.
4. **No test for malformed SSE events** — No garbage `data:` lines in test inputs.
5. **No test for double-finalize** — No scenario that sends `[DONE]` AND calls `parser.finalize()`.
6. **`raw_body_` grows unbounded** — All SSE bytes appended to `raw_body_` for diagnostics, but only 400-byte snippet is used. Wasted memory on long streams.
