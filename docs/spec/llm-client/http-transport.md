## Spec: HTTP Transport (libcurl)

### Purpose
Send JSON payloads to the OpenAI-compatible LLM API endpoint and return the
response. Supports both buffered (`POST`) and streaming (`SSE`) modes.
Handles authentication, timeout, cancellation, and HTTP-level errors.

### Ownership
- **Source files**: `lib/http_transport.cpp`, `lib/http_transport.h` (RAII wrappers), `lib/request_builder.cpp` (`build_chat_body`), `lib/llm.cpp` (`chat()`, `chat_stream()` — callers)
- **NB**: This file lives in `lib/` (core) but its header is NOT in `include/agent/` — a build-layer quirk. Transport internals are not exposed outside `lib/`.
- **Test files**: No direct tests. Indirectly tested via SSE parser tests, config tests, and mock-SSE integration tests.

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | `Config` (api_base, api_key, model, timeout) + JSON payload string + SSE mode flag |
| **Output** | Buffered: raw response `string`. Streaming: via `StreamParser` callbacks. Both: `Stats` populated with latency/tokens/TPS. |
| **Error states** | cURL init failure → throw. Connection/HTTP error → throw with diagnostic. Cancellation → throw `CURLE_ABORTED_BY_CALLBACK`. |
| **Invariants** | See below. |
| **Thread safety** | `curl_easy_perform()` blocks the calling thread. Cancellation via `cancel_check_cb` (curl progress callback) polls a shared `CancellationToken`. |

### Invariants

1. Every request includes `Authorization: Bearer <key>` if `cfg.api_key` is non-empty.
2. Every request has a 300-second absolute timeout.
3. Streaming requests also have a 60-second low-speed timeout (<1 byte/sec = abort).
4. The cancel-check callback is always registered for both streaming and buffered (`CURLOPT_NOPROGRESS = 0L`).
5. HTTP status outside 200–299 always throws with the response body excerpt.
6. The `CURL*` handle is owned by `unique_ptr` with `curl_easy_cleanup` — exception-safe.
7. The `curl_slist*` header list is owned by RAII `HeaderList` struct — exception-safe.
8. `model_probe.cpp` is the ONLY file that still uses raw `curl_easy_init/cleanup` (known gap).
9. JSON serialisation uses `error_handler_t::replace` to prevent UTF-8 exceptions.

---

### Scenarios

#### [HT-01] Successful buffered request

- **Given**: Valid config, reachable server
- **Input**: `post_completion(cfg, payload, accept_sse=false, &ttfb, &total)`
- **Expected**: Returns raw response body string. `ttfb` and `total` populated with seconds. No exception.
- **On failure**: Connection refused → `CURLE_COULDNT_CONNECT` → throw.

#### [HT-02] Successful streaming request

- **Given**: Valid config, reachable server
- **Input**: `stream_completion(cfg, payload, parser, stats, status)`
- **Expected**: `parser.on_write()` called for each SSE chunk. `parser.finalize()` called after transfer. `stats` populated with tokens/latency/TPS. `status` set to HTTP code.
- **On failure**: Same as buffered.

#### [HT-03] HTTP 4xx error (bad request, auth failure)

- **Given**: Wrong API key or malformed request
- **Input**: Server returns HTTP 401 or 400
- **Expected**: Buffered: `post_completion()` throws with `"HTTP 401 from LLM server: <first 200 bytes>"`. Streaming: `stream_completion()` throws with `"HTTP 400 from LLM server: <first 400 bytes>"`.
- **Recovery**: Error caught by `safe_chat_once()`, returned as error message to agent. No retry at transport layer.

#### [HT-04] HTTP 5xx error (server error)

- **Given**: Server overloaded or crashing
- **Input**: HTTP 502 or 503
- **Expected**: Same as [HT-03]. 300s timeout also applies if server hangs.
- **Recovery**: Agent loop retries once (not transport layer).

#### [HT-05] Connection timeout (300s)

- **Given**: Server does not respond within 300 seconds
- **Input**: `CURLOPT_TIMEOUT = 300L`
- **Expected**: `CURLE_OPERATION_TIMEDOUT` → throw.
- **On failure**: Connection hangs indefinitely.

#### [HT-06] Low-speed timeout (streaming only)

- **Given**: Server sends initial headers then stops sending data
- **Input**: `<1 byte/sec for 60 seconds`
- **Expected**: `CURLE_OPERATION_TIMEDOUT` → throw.
- **On failure**: Connection hangs for 300s (absolute timeout) instead of 60s.

#### [HT-07] Cancel during transfer

- **Given**: `cfg.cancel_token.request()` called during transfer
- **Input**: `CURLOPT_XFERINFOFUNCTION` = `cancel_check_cb`
- **Expected**: `cancel_check_cb` returns 1 on next poll. `CURLE_ABORTED_BY_CALLBACK` → throw.
- **On failure**: Transfer completes uninterrupted.

#### [HT-08] Empty API key — no auth header

- **Given**: `cfg.api_key` is empty
- **Input**: Any request
- **Expected**: No `Authorization` header sent. Connection proceeds without auth.
- **Regression guard**: Tests for proxy/local endpoints.

#### [HT-09] Trailing slash in api_base

- **Given**: `cfg.api_base = "http://host:8080/v1/"` (trailing slash)
- **Input**: `cfg.api_url()` = `api_base + "/chat/completions"`
- **Expected**: Config validation REJECTS trailing slashes. If bypassed programmatically, URL becomes `http://host:8080/v1//chat/completions` (double slash).
- **Regression guard**: `config_validate_flags_problems` test.

#### [HT-10] Request body with invalid UTF-8

- **Given**: Conversation history contains invalid UTF-8 bytes
- **Input**: `body.dump(-1, ' ', false, json::error_handler_t::replace)`
- **Expected**: Invalid sequences replaced with U+FFFD. `json::type_error.316` is never thrown.
- **Regression guard**: `request_body_survives_invalid_utf8` test.

#### [HT-11] Request body with empty assistant content but tool calls

- **Given**: Assistant message has `tool_calls` but empty `content`
- **Input**: `build_chat_body()` serialises the message
- **Expected**: `"content": ""` is explicitly emitted even when empty. Prevents HTTP 400 from servers that require the `content` field.
- **Regression guard**: `request_builder_assistant_message_always_has_content` test.

#### [HT-12] cURL init failure

- **Given**: `curl_easy_init()` returns NULL (extreme resource exhaustion)
- **Input**: Any request
- **Expected**: `throw std::runtime_error("curl_easy_init failed")`.
- **On failure**: Null dereference.

#### [HT-13] Non-JSON response (server returns HTML)

- **Given**: Misconfigured server or proxy returns HTML error page
- **Input**: `message_from_completion(html_string)`
- **Expected**: `json::parse` returns discarded. `out.content = "[error: malformed LLM response, raw body follows]\n<html>..."`. Message returned to agent for self-recovery.
- **On failure**: `json::parse` throws uncaught exception.

#### [HT-14] Tool call arguments validation (buffered path)

- **Given**: Server returns tool calls with non-JSON arguments
- **Input**: `message_from_completion()` parsing response
- **Expected**: Same validation as streaming path: if ANY call has non-JSON `arguments`, ALL tool calls discarded. Message proceeds as text-only.
- **DRY violation**: This validation is duplicated in `chat_stream()` (`lib/llm.cpp:73-88`) and `message_from_completion()` (`lib/http_transport.cpp:84-96`).

#### [HT-15] Streaming with large tool call arguments

- **Given**: A tool call with many arguments spread across many SSE fragments
- **Input**: Stream with 10+ fragments, each containing partial arguments
- **Expected**: All fragments concatenated correctly via `accumulate_arguments()`. Final arguments string is valid JSON. No partial state leaks between SSE events.

#### [HT-16] Buffered response with usage stats

- **Given**: Server returns `usage` in the response JSON
- **Input**: `message_from_completion()` + `fill_buffered_stats()`
- **Expected**: `stats.prompt_tokens`, `stats.completion_tokens`, `stats.latency_ms` populated. `stats.tps = completion_tokens / (total - ttfb)`.
- **On failure**: Stats not populated, or division by zero if `total == ttfb`.

---

### Cross-references

- **Depends on**: `llm-client/streaming.md` (SSE parser consumed by streaming path), `workspace/security-model.md` (no dependency — auth is API key based)
- **Depended on by**: `agent-loop/core-loop.md` (LLM calls), `llm-client/model-probe.md` (separate transport, shares no code)
- **Test coverage**: No direct transport tests. Indirect: `tests/run_tests.cpp` — `config_validate_flags_problems`, `request_body_survives_invalid_utf8`, `request_builder_assistant_message_always_has_content`, `cancel_token_*` (5 tests)

### Known gaps

1. **No direct tests for `http_transport.cpp`** — `post_completion()`, `stream_completion()`, `curl_exec()`, `message_from_completion()`, and `fill_buffered_stats()` have zero unit test coverage.
2. **`model_probe.cpp` leaks `CURL*` on exception** — Uses raw `curl_easy_init()/cleanup()` without RAII. The `http_transport.*` RAII fix was NOT applied to model probe.
3. **DRY violation: tool call validation duplicated** — Same non-JSON argument check in `message_from_completion()` (buffered) and `chat_stream()` (streaming).
4. **300s timeout is identical for buffered and streaming** — The ternary `accept_sse ? 300L : 300L` is vestigial (was 900L for streaming).
5. **`CURLOPT_POST` not explicitly set** — libcurl infers POST from `CURLOPT_POSTFIELDS`, but the behaviour is not documented as guaranteed by curl.
6. **`raw_body_` overhead on error** — Large response bodies that cause HTTP errors are fully captured in `parser.raw_body_` but only 400 bytes are used for the error message.
