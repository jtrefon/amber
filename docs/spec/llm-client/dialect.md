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
- **Expected**: One top-level `system` field (a block array with a
  `cache_control` breakpoint on the stable block when prompt caching is on, a
  merged string when it is off); `text`/`tool_use` (parsed `input`)/`tool_result`
  blocks; `input_schema` tools; streamed named events decoded to the internal
  `StreamChunk`; `input_tokens`/`output_tokens` plus the cache read/write
  counters mapped to `TokenUsage`.
- **Regression guard**: `tests/dialect_anthropic_test.cpp` (no network) and the
  cache pins in `tests/run_tests.cpp` (see
  `docs/spec/llm-client/prompt-caching.md`).

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

#### [DL-08] Anthropic replay repairs the tool pairing contract

- **Given**: A stored history that predates the live turn — an assistant tool
  turn whose ids were never answered, a call that carries no id, or a stray
  tool_result
- **Input**: `build_chat_body` over that history
- **Expected**: Every `tool_use` id is answered by a `tool_result` in the
  immediately-following user message — unanswered ids close in a user message
  of their own, before the next assistant turn; an id-less call gets a stable
  synthesized id its result echoes; a tool_result no tool_use asked for is
  dropped. The body never carries a `tool_result` block in an assistant
  message, an empty id, or an empty content array. One unrepaired replay 400s
  every later request in the session, which is why the walk repairs rather
  than trusts the history it is handed.
- **Regression guard**: `anthropic_unanswered_turn_closes_before_the_next_assistant_message`,
  `anthropic_wire_synthesizes_missing_tool_ids`,
  `anthropic_wire_drops_tool_results_nobody_asked_for` tests.

#### [DL-09] The listing carries the limits and the thinking contract

- **Given**: An Anthropic Models API response (`max_input_tokens`, `max_tokens`,
  `line`, `capabilities.thinking.types`) or an OpenAI-compatible gateway that
  lists `anthropic/claude-*` with limits omitted
- **Input**: `parse_model_list_response` / `parse_models_response`
- **Expected**: The output ceiling is read from `max_tokens` (the documented
  field), falling back to `max_output_tokens`; the input window from
  `max_input_tokens`. A documented Claude family fills only fields the listing
  omitted, matched on the basename by longest prefix (`claude-opus-4-5` never
  resolves as `claude-opus-4`), and the family carries the thinking mode
  (adaptive, legacy budget, always-on), the accepted effort levels and their
  default. A `capabilities.thinking.types` entry that says `supported: false`
  overrides the family default. An unknown id stays 0/false: unknown is never
  guessed. The 5.x/4.6+ families are 1M/128K adaptive; 4.5 is 200K/64K legacy;
  4.1/4.0 are 200K/32K; 3.x keeps its documented limits.
- **Regression guard**: `anthropic_model_list_reads_limits_capabilities_and_efforts`,
  `anthropic_model_list_reports_the_input_window`,
  `anthropic_known_family_fallback_is_bounded`,
  `probe_parse_model_list_fills_documented_claude_families` tests.

#### [DL-10] The request uses the model's reasoning shape, not a guess

- **Given**: A model whose listing established a reasoning contract, and a
  configured effort (`off|low|medium|high|xhigh|max`)
- **Input**: `build_chat_body`
- **Expected**: Adaptive models send `thinking:{type:"adaptive"}` plus
  `output_config.effort` when the effort is listed (otherwise the model's
  documented default); they never send `budget_tokens`. Legacy models send
  `thinking:{type:"enabled",budget_tokens}` (kept below `max_tokens`) and, when
  the model lists efforts (Opus 4.5), the clamped effort too. "Off" is sent as
  `disabled` or `between_tools` only when the capability advertises that type;
  an always-on model is left with its default instead of receiving a block the
  API rejects. A model whose listing said nothing about thinking gets no
  block. The adaptive Claude families also reject non-default sampling
  temperature, so the OpenAI-compatible dialect omits it for them (amber's 0.2
  would 400 on every request); every other model keeps the configured value.
- **Regression guard**: `anthropic_request_uses_adaptive_thinking_and_effort`,
  `anthropic_request_off_on_always_on_model_never_sends_disabled`,
  `anthropic_request_legacy_budget_and_effort`,
  `anthropic_request_respects_output_and_reasoning`,
  `openai_request_omits_temperature_only_for_adaptive_claude` tests.

#### [DL-11] Thinking blocks survive the tool round trip

- **Given**: An assistant turn that contains thinking blocks (with signatures)
  and tool_use blocks, from a buffered response or an SSE stream
- **Input**: `parse_completion` then `build_chat_body` with that turn and its
  tool result
- **Expected**: The protocol's own content blocks travel with the `Message`
  (`raw_content`); the next assistant message replays them verbatim. The API
  requires the signed thinking blocks back unmodified, so reconstructing the
  turn from text plus parsed arguments is a 400 on every later request in the
  session. Text-only assistant turns use the same raw blocks when present.
- **Regression guard**: `anthropic_thinking_blocks_survive_the_tool_round_trip`
  test.

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
| 2026-10-21 | DL-09..11: documented 5.x/4.6+ limits and thinking contract, adaptive/legacy/off request shapes and effort clamping, thinking-block round trip, OpenAI Claude-family fallback and sampling guard; catalogue TTL 6h |
