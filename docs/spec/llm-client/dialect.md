## Spec: Provider Dialects (wire protocols)

### Purpose
Isolate everything that differs between LLM provider wire protocols, endpoints,
auth headers, request body shape, buffered response parsing, streamed decoding,
model listing, usage mapping, and error classification, behind one port.
The transport, agent loop, and UIs speak only the internal message model;
adding a provider protocol is a new `Dialect` implementation plus one row in
the registry. OpenAI-compatible endpoints (llama.cpp, vLLM, Ollama, OpenRouter,
kilocode, …) need no code at all, they are config-only under the `openai`
dialect.

### Ownership
- **Source files**: `include/agent/dialect.h` (port + `TokenUsage` + registry),
  `lib/dialect.cpp` (flavor→factory table + openai fallback),
  `lib/dialect_openai.{h,cpp}`, `lib/dialect_anthropic.{h,cpp}`,
  `include/agent/stream_decoder.h` + `lib/stream_decoder.cpp` (shared SSE
  framing base).
- **Consumers**: `lib/llm.cpp` (`HttpLLMClient` resolves and drives one dialect),
  `lib/http_transport.cpp` (URL, auth headers, error classification),
  `lib/model_probe.cpp` (model listing), `lib/providers_service.cpp`
  (`apply_selection` sets `Config::flavor` from the provider's capabilities).
- **Test files**: `tests/dialect_anthropic_test.cpp` (hermetic protocol tests)
  and the wire pins in `tests/run_tests.cpp`.

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | `Config` (flavor, api_base, api_key, model, tuning knobs), internal `Message` history, `Tool` set |
| **Output** | Request `json`; buffered `Message`; `StreamChunk` per SSE event; `ServerInfo` / `ModelInfo` listing; `TokenUsage` |
| **Error states** | Parsing degrades, never throws: a malformed buffered body becomes the recovery message (`"[error: malformed LLM response…]"`), a malformed streamed event is dropped. Transport/HTTP failures are classified by `is_retryable` and thrown as typed `ApiError` by the transport. |
| **Invariants** | See below. |
| **Thread safety** | Dialects are stateless and shared per client; decoders are single-threaded (fed by the curl write callback). |

### Invariants

1. Flavor resolves **exactly once** per client construction
   (`make_dialect(cfg.flavor)`); no downstream code branches on flavor or
   provider names.
2. Unknown flavors fall back to `"openai"`, a typo never breaks a session.
3. Tool calls normalize to the internal shape
   (`{id, type:"function", function:{name, arguments}}`, arguments as a JSON
   *string*) at the dialect edge, for buffered and streamed responses alike.
4. Placeholder/nameless tool calls are dropped before entering history
   (dispatch would deny an unknown tool and poison replay).
5. Decoders emit the terminal `done` chunk exactly once; `[DONE]`/`message_stop`
   and the post-transfer `finalize()` are idempotent.
6. The `StreamDecoder` base owns framing (line splitting, raw-byte capture,
   debug logging, terminal chunk); subclasses own only `decode_payload` and
   `decode_end`.
7. `auth_headers` never embeds an empty credential; header names/shapes are the
   protocol's (Bearer, `x-api-key`, …).
8. Adding a dialect touches no transport, agent-loop, or UI file.

---

### Scenarios

#### [DL-01] Registry resolution and fallback

- **Given**: A client constructed for a provider
- **Input**: `make_dialect("openai")`, `make_dialect("anthropic")`, `make_dialect("nonsense")`
- **Expected**: The matching dialect, the matching dialect, and the openai dialect respectively.
- **Regression guard**: `dialect_registry_resolves_and_falls_back` test.

#### [DL-02] Registering a new dialect

- **Given**: A new `Dialect` implementation
- **Input**: `register_dialect("x", factory)` then `make_dialect("x")`
- **Expected**: The registered dialect is returned, the extension point a
  future plugin `Provider` capability uses.
- **Regression guard**: `dialect_registry_accepts_new_factories` test.

#### [DL-03] OpenAI-compatible provider (config only)

- **Given**: A user provider file pointing at any OpenAI-compatible endpoint
- **Input**: Config with `flavor == "openai"` (the default)
- **Expected**: Endpoints `{api_base}/chat/completions` + `{api_base}/models`,
  `Authorization: Bearer`, OpenAI body/SSE shapes. No code change needed to add
  such a provider.
- **Regression guard**: the wire pins in `tests/run_tests.cpp`.

#### [DL-04] Anthropic Messages API dialect

- **Given**: A provider with `flavor == "anthropic"`
- **Input**: A conversation with system messages, tool calls, and tool results
- **Expected**: One merged top-level `system` string; `text`/`tool_use`
  (parsed `input`)/`tool_result` blocks; `input_schema` tools; streamed named
  events decoded to the internal `StreamChunk`; `input_tokens`/`output_tokens`
  mapped to `TokenUsage`.
- **Regression guard**: `tests/dialect_anthropic_test.cpp` (12 tests, no network).

#### [DL-05] Auth header construction

- **Given**: A dialect and a config with and without an API key
- **Input**: `auth_headers(cfg)`
- **Expected**: No credential header when the key is empty; exactly one
  protocol-shaped credential header when set.
- **Regression guard**: `apply_auth_emits_bearer_only_with_key` (openai),
  `anthropic_auth_headers` tests.

#### [DL-06] Error classification per protocol

- **Given**: A non-2xx response
- **Input**: `is_retryable(status, body)`
- **Expected**: Transient failures (429, 5xx, including Anthropic's 529
  overloaded; openai's empty-SSE-stream 400) are retryable; genuine rejections
  (JSON error 400, 401/403) are not.
- **Regression guard**: `http_error_empty_stream_400_is_retryable`,
  `http_error_json_400_is_not_retryable`, `anthropic_error_classification` tests.

#### [DL-07] Overflow teaching

- **Given**: A 400 whose body names the server's real context window
- **Input**: `context_overflow_hint(body)`
- **Expected**: The enforced window (e.g. `"n_ctx is 2048"` → 2048,
  `"… 213456 tokens > 200000 maximum"` → 200000), or 0 when unknown, never a
  guess. The transport stores it and the client surfaces it through
  `learned_context_size()` even though the request threw.
- **Regression guard**: `dialect_context_overflow_hint_patterns`,
  `overflow_400_streaming_throws_non_retryable_api_error`,
  `overflow_400_buffered_throws_non_retryable_api_error` tests.

---

### Cross-references

- **Depends on**: `llm-client/http-transport.md` (curl mechanics),
  `llm-client/streaming.md` (framing contract), `llm-client/model-probe.md`
  (listing consumers)
- **Depended on by**: `plugins/plugin-framework.md` (the `Provider` plugin
  capability registers a dialect factory, PF-2 in
  `docs/plugin-framework-tracker.md`), provider selection
  (`ProviderCapabilities::flavor`)
- **Test coverage**: `tests/dialect_anthropic_test.cpp` (DL-01..07 protocol),
  `tests/run_tests.cpp` wire pins (openai paths + cancellation + overflow)

### Revision history

| Date | Reason |
|------|--------|
| 2026-09-09 | Initial spec (FIX-027..032, dialect seam, capability wiring, Anthropic proof, learned-window fix) |
