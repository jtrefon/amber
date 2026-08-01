## Spec: LLM Client Error Handling

### Purpose
Define how errors from the LLM API (network failures, HTTP errors, malformed
responses) are surfaced to the agent loop. The transport layer throws on
recoverable errors; `safe_chat_once()` catches and converts them to structured
error messages the agent can forward to the model for self-recovery.

### Ownership
- **Source files**: `lib/agent_helpers.cpp` (`safe_chat_once()` — lines 151–171), `lib/llm.cpp` (`chat()`, `chat_stream()`), `lib/http_transport.cpp` (`curl_exec()`, `post_completion()`, `stream_completion()`)
- **Test files**: No direct tests.

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | Exceptions from HTTP transport, JSON parse failures, tool call validation |
| **Output** | `Message` with content starting with `"[error during <stage>: <details>]"` |
| **Error states** | See scenarios. |
| **Invariants** | See below. |
| **Thread safety** | Agent thread only. |

### Invariants

1. All transport errors throw `std::runtime_error` — never return error codes.
2. `safe_chat_once()` catches ALL exceptions and returns an error message — never lets exceptions propagate to the agent loop.
3. `chat_with_retry` retries transient failures up to 3 attempts with
   1s→2s exponential backoff (cancellable, 100 ms slices polling the shared
   CancellationToken). Retryable: transport/curl errors, timeouts, empty
   bodies, HTTP 429/502/503/504. Non-retryable (auth/misconfig 4xx, malformed
   bodies): fail fast. On exhaustion the standard `"[error during"` message
   is returned and the loop degrades gracefully (see
   `llm-client/agent-loop-reliability.md`).
4. Malformed JSON responses return `"[error: malformed LLM response]"` with raw body — not a throw.
5. Tool calls with non-JSON arguments are silently discarded (with debug log) — not an error.

---

### Scenarios

#### [EH-01] Connection refused

- **Given**: LLM server unreachable
- **Input**: `curl_easy_perform` returns `CURLE_COULDNT_CONNECT`
- **Expected**: `curl_exec()` throws `"curl error: Couldn't connect to server"`. `safe_chat_once()` catches → returns `Message{"[error during generation: curl error: Couldn't connect to server]"}`.
- **On failure**: Exception propagates to agent loop.

#### [EH-02] HTTP 401 (auth failure)

- **Given**: Wrong API key
- **Input**: LLM server returns HTTP 401
- **Expected**: `post_completion()` throws `"HTTP 401 from LLM server: <body snippet>"`. Same catch path.
- **On failure**: Raw HTTP status passed through.

#### [EH-03] HTTP 5xx (server error)

- **Given**: Server overloaded
- **Input**: HTTP 502
- **Expected**: Same as EH-02. Error message includes first 200 bytes of response body (buffered) or 400 bytes (streaming).

#### [EH-04] Timeout

- **Given**: Server does not respond within 300s
- **Input**: `CURLE_OPERATION_TIMEDOUT`
- **Expected**: `"curl error: Timeout was reached"`. Included in error message.

#### [EH-05] Malformed JSON response

- **Given**: Server returns non-JSON body
- **Input**: `message_from_completion("<html>...</html>")`
- **Expected**: `json::parse` returns discarded. `"[error: malformed LLM response, raw body follows]\n<html>..."`. Message returned to agent — not thrown.
- **On failure**: `json::parse` throws `type_error`.

#### [EH-06] Tool call with non-JSON arguments

- **Given**: Assistant message has tool calls with bad arguments
- **Input**: `"arguments": "not json"`
- **Expected**: `chat_stream()` validates post-stream. Non-JSON found → ALL tool calls set to null. Message proceeds as text-only. Debug log: `"discarding malformed tool calls"`.
- **On failure**: Bad arguments reach tool dispatch, causing parse error there.

#### [EH-07] UTF-8 encoding error in request body

- **Given**: History contains invalid UTF-8
- **Input**: `body.dump(-1, ' ', false, json::error_handler_t::replace)`
- **Expected**: Invalid sequences replaced with U+FFFD. `json::type_error.316` never thrown.
- **On failure**: Serialization throws, caught by `chat_once` or propagates to `safe_chat_once`.

#### [EH-08] Retry once on LLM error

- **Given**: First LLM call fails with `"[error during...]"`
- **Input**: Agent loop checks for error prefix
- **Expected**: Retries once with same history (no compression). If retry succeeds, continues normally. If retry also fails, error message pushed to history and loop continues.
- **On failure**: No retry, or compression destroys KV cache before retry.

#### [EH-09] Tool execution exception

- **Given**: `Tool::execute()` throws
- **Input**: Any tool call
- **Expected**: `std::async` lambda catches: `catch (...) { return ToolResult{false, "", "tool threw: ..."}; }`. Result returned as normal `ToolResult{ok=false}`.
- **On failure**: Unhandled exception terminates agent thread.

---

### Cross-references

- **Depends on**: `llm-client/http-transport.md`, `llm-client/streaming.md`, `agent-loop/core-loop.md` (retry logic)
- **Depended on by**: `docs/spec/INDEX.md`
- **Test coverage**: No direct error-handling tests.

### Known gaps

1. **No transport-level retry** — Retry is only at the agent loop level (one attempt). Transport layer never retries.
2. **No exponential backoff** — Retry is immediate. No backoff for rate-limited (HTTP 429) responses.
3. **No circuit breaker** — Repeated connection failures keep retrying until `max_tool_iterations`.
4. **`raw_body_` overhead** — Large error responses are fully buffered but only 400 bytes are used for diagnostics.
