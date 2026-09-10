## Spec: Model Probe (Server Auto-Detection)

### Purpose
Query the provider's model-listing endpoint to auto-detect the model name and
context window size. The endpoint, auth headers, and response parsing come
from the resolved **dialect** (see `llm-client/dialect.md`): the openai dialect
handles the OpenAI / llama.cpp / Ollama listing shapes, and a protocol without
a listing endpoint returns an empty/not-ok result rather than a false negative.
Results are merged into Config only for fields NOT marked explicit by the user.

### Ownership
- **Source files**: `lib/model_probe.cpp`, `include/agent/model_probe.h`; parsing lives in the dialects (`Dialect::parse_models_response` / `parse_model_list_response`)
- **Consumers**: `LLMClient::probe_server()`, `agent::list_model_info()` / `list_models()` (TUI model drawer, `/provider test` via `HttpModelCatalog`)
- **Test files**: `tests/run_tests.cpp` — probe pins (`probe_parse_*`, `probe_prefers_*`, `probe_autodetect_*`, `probe_active_model_without_meta_is_unknown`) and `tests/dialect_anthropic_test.cpp` (`anthropic_model_list_and_probe_parse`)

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | `Config` with `api_base`, optional `api_key`, and `flavor`; the dialect derives the listing URL and auth |
| **Output** | `ServerInfo{ok, model, context_size}` from the dialect's parsing of the listing response |
| **Error states** | Connection failure → `ok=false`. Malformed JSON → `ok=false`. Empty data → `ok=false`. |
| **Invariants** | See below. |
| **Thread safety** | Startup only. |

### Invariants

1. Model name resolution priority: `data[0].id` (OpenAI) > `data[0].model` > `data[0].name` (Ollama) > `models[0].name`.
2. Context size resolution: `meta.n_ctx` (llama.cpp) > top-level `n_ctx` (Ollama) > `meta.n_ctx_train` > `n_ctx_train`.
3. Probe returns `ok=true` only if at least model name or context size is found.
4. Probes use raw `curl_easy_init/cleanup` (NOT RAII — known leak on exception).
5. Timeout: 10s total, 5s connect.

---

### Scenarios

#### [MP-01] OpenAI format — `data[].id` + `meta.n_ctx`

- **Given**: Server returns OpenAI-compatible JSON
- **Input**: `{"object":"list","data":[{"id":"gpt-4","meta":{"n_ctx":8192}}]}`
- **Expected**: `info.model = "gpt-4"`, `info.context_size = 8192`, `info.ok = true`.
- **Regression guard**: `probe_parse_models_data_array` test.

#### [MP-02] Ollama format — `models[].name` + `n_ctx`

- **Given**: Server returns Ollama-compatible JSON
- **Input**: `{"models":[{"name":"llama3.2","n_ctx":8192}]}`
- **Expected**: `info.model = "llama3.2"`, `info.context_size = 8192`, `info.ok = true`.
- **Regression guard**: `probe_parse_models_array_fallback` test.

#### [MP-03] Malformed response

- **Given**: Server returns non-JSON or empty
- **Input**: `"Internal Server Error"`
- **Expected**: `json::parse` returns discarded. `info.ok = false`. Config unchanged.
- **Regression guard**: `probe_parse_models_malformed_is_not_ok` test.

#### [MP-04] Server unreachable

- **Given**: Server down or connection refused
- **Input**: `curl_easy_perform` fails
- **Expected**: `probe_server()` returns `ok=false` (curl error caught). Config unchanged.
- **Regression guard**: `autodetect_noop_when_server_down` test.

#### [MP-05] Auto-detect only fills non-explicit fields

- **Given**: `model_explicit=true` (user set model), `context_explicit=false`
- **Input**: Server reports `model=llama, ctx=8192`
- **Expected**: `merge_server_info()` → model NOT overwritten, context_size filled.
- **Regression guard**: `autodetect_fills_only_auto_fields` test.

#### [MP-06] `list_models()` returns all model IDs

- **Given**: Server with multiple models
- **Input**: `{"data":[{"id":"gpt-4"},{"id":"gpt-3.5"}]}`
- **Expected**: `parse_model_list()` returns `["gpt-4", "gpt-3.5"]`.
- **On failure**: Only first model returned.

---

### Cross-references

- **Depends on**: `llm-client/dialect.md` (endpoint, auth, parsing), `llm-client/http-transport.md` (curl usage — but probe uses raw curl, not RAII)
- **Depended on by**: `config/merge-semantics.md`, `config/ui-config.md` (settings screen test connection)
- **Test coverage**: `tests/run_tests.cpp`: `probe_parse_llamacpp_models`, `probe_parse_models_array_fallback`, `probe_parse_models_malformed_is_not_ok`, `probe_prefers_the_active_model`, `probe_autodetect_prefers_explicit_active_model`, `probe_autodetect_first_with_context_when_auto`, `autodetect_noop_when_server_down`

### Known gaps

1. **Raw CURL without RAII** — `CURL*` and `curl_slist*` are raw pointers. Exception between init and cleanup leaks handles. Contrast with `http_transport.cpp` which uses `unique_ptr`.
2. **No cancellation support** — Probe requests lack `CURLOPT_XFERINFOFUNCTION` — cannot be cancelled via `CancellationToken`.
3. **No HTTP status check** — Even a 404/500 response body is passed to `parse_models()` and parsed (likely returning `ok=false`, but no explicit check).
