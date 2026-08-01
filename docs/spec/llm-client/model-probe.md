## Spec: Model Probe (Server Auto-Detection)

### Purpose
Query the LLM server's `/v1/models` endpoint to auto-detect the model name and
context window size. Handles three JSON response formats (OpenAI, llama.cpp,
Ollama). Results are merged into Config only for fields NOT marked explicit by
the user.

### Ownership
- **Source files**: `lib/model_probe.cpp` (155 lines), `include/agent/model_probe.h`, `lib/llm.cpp` (`LLMClient::probe_server()`, `LLMClient::list_models()`)
- **Test files**: `tests/run_tests.cpp` — 4 probe tests (lines 667–735)

---

### Contract

| Dimension | Detail |
|-----------|--------|
| **Input** | `Config` with `api_base` and optional `api_key` |
| **Output** | `ServerInfo{ok, model, context_size}` — populated from `/v1/models` response |
| **Error states** | Connection failure → `ok=false`. Malformed JSON → `ok=false`. Empty data → `ok=false`. |
| **Invariants** | See below. |
| **Thread safety** | Startup only. |

### Invariants

1. Model name resolution priority: `data[0].id` (OpenAI) > `data[0].model` > `data[0].name` (Ollama) > `models[0].name`.
2. Context size resolution: `meta.n_ctx` (llama.cpp) > top-level `n_ctx` (Ollama) > `meta.n_ctx_train` > `n_ctx_train`.
3. Probe returns `ok=true` only if at least model name or context size is found.
4. Probes use raw `curl_easy_init/cleanup` (NOT RAII — known leak on exception).
5. Timeout: 5s total, 3s connect.

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

- **Depends on**: `llm-client/http-transport.md` (curl usage — but probe uses raw curl, not RAII)
- **Depended on by**: `config/merge-semantics.md`, `config/ui-config.md` (settings screen test connection)
- **Test coverage**: `tests/run_tests.cpp`: `probe_parse_models_data_array` (668), `probe_parse_models_array_fallback` (679), `probe_parse_models_malformed_is_not_ok` (689), `autodetect_fills_only_auto_fields` (697)

### Known gaps

1. **Raw CURL without RAII** — `CURL*` and `curl_slist*` are raw pointers. Exception between init and cleanup leaks handles. Contrast with `http_transport.cpp` which uses `unique_ptr`.
2. **No cancellation support** — Probe requests lack `CURLOPT_XFERINFOFUNCTION` — cannot be cancelled via `CancellationToken`.
3. **No HTTP status check** — Even a 404/500 response body is passed to `parse_models()` and parsed (likely returning `ok=false`, but no explicit check).
