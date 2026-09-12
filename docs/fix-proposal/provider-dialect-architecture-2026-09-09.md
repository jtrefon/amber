# Provider Dialect Architecture Proposal, 2026-09-09

- **Status:** All implemented (FIX-027..032), PR [#99](https://github.com/jtrefon/amber/pull/99), squash-merged to `main` as `e7ecfa4`; all CI checks green
- **Branch:** `refactor/provider-dialect-seam` (merged); follow-up hygiene in `chore/no-debt-cleanup`
- **Author:** Session analysis 2026-09-09 (3 parallel explorers + manual file verification)
- **Target:** A provider layer that grows by *registration*, not by branching, any OpenAI-compatible endpoint stays config-only; genuinely different wire protocols (Anthropic, Gemini, …) become one dialect adapter each, touching zero shared code. Refactor is regression-protected by **characterization pins on the moving surface**, not by blocking on whole-app coverage.
- **Constraints:** No `llama-turboq:8081` service changes; no live-API calls in the hermetic suite; plugin v2 `Capability` stays `void*` until a consumer exists (consistent with prior YAGNI sign-off); every phase Red → Green per `AGENTS.md` fix workflow (FIX-027 characterization is green-by-construction, see §3.4).
- **References:** `AGENTS.md` (Engineering principles, fix workflow, context stack), `docs/fix-tracker.md` (FIX-027..032 follow FIX-026 render-engine), `docs/issues.md`, `docs/fix-proposal/clean-architecture-2026-08-27.md` (prior style + decisions), `docs/spec/plugins/plugin-framework.md` (Phase 5 Provider capability), `docs/spec/llm-client/{dialect,http-transport,model-probe,streaming}.md`, `include/agent/{llm,providers,config,dialect,stream_decoder,model_probe}.h`

---

## 1. Executive Summary

The provider story has **two clean halves and one missing middle**:

- **Clean half 1, the agent port.** `LLMClient` (`include/agent/llm.h:71`) is a genuine interface with provider-agnostic DTOs (`Message`, `StreamChunk`, `Stats`, `ServerInfo`); `Agent` consumes it through an injectable `LLMClientFactory` (`include/agent/agent.h:50`). Tests prove a full alternative client can slot in (`tests/fake_llm.h`).
- **Clean half 2, the provider data domain.** `Provider` / `ProviderRepository` / `ModelCatalog` / `ProviderService` (`include/agent/providers.h`) is a properly hexagonal registry of provider *definitions* (presets + `~/.config/amber/providers/*.conf`), surfaced through `/provider` commands and feed leaves. No name branching lives in the domain.
- **Missing middle, the wire dialect.** The only production client, `HttpLLMClient` (`lib/llm.cpp:14`), is a monolithic **OpenAI `/chat/completions` adapter**: URL derivation (`config.h:180`), Bearer auth (`http_transport.cpp:95`), request body (`request_builder.cpp:75`), buffered parse (`http_transport.cpp:115`), SSE decode (`sse_parser.cpp:146`), model probe (`model_probe.cpp:59`), error classification (`http_transport.cpp:56,160`) are all hardwired to one wire format. Provider identity collapses into strings in `Config` and nothing downstream consults it.

The declared divergence mechanism, `ProviderCapabilities { bearer_auth, supports_reasoning_effort, flavor }` (`providers.h:40`), is **dead code**: its name-keyed table (`providers_service.cpp:12`) has zero production consumers.

**Verdict: we are pre-explosion (no provider `if/switch` exists anywhere, good), but growth past the OpenAI-compatible family requires editing four shared files.** That is exactly the moment to introduce the dialect seam, while the OpenAI behavior is the only behavior and the refactor is a pure move.

**Regression strategy in one line: do NOT block on full app coverage. Pin the exact moving surface with characterization tests first (FIX-027), then move code+tests together with an "assertions unchanged" review gate.** The wire layer already has meaningful unit + end-to-end-mock coverage (§2.3); the gap list is small, concrete, and closed by FIX-027 before any production line moves.

This proposal introduces one port, `Dialect`, a Strategy for everything that differs between wire protocols, resolved **once per client construction** from a `flavor` string that `apply_selection` copies into `Config`. Dispatch happens exactly once (registry lookup by flavor); downstream code is pure virtual dispatch. The current OpenAI implementation becomes the default dialect via **pure moves** (zero behavior change, pins + existing tests are the regression net). One real second dialect (Anthropic Messages API) is built **hermetic-only** as the growth proof. A provider is then: *one dialect file + one capability-table row + (optionally) one preset row*, no shared-file edits.

**Done when:** the pin suite (FIX-027) passes unchanged apart from mechanical renames across the move; an Anthropic-flavored provider works end-to-end against hermetic fixtures with zero changes to `lib/http_transport.cpp`, `lib/sse_parser.cpp`, `lib/model_probe.cpp`, `lib/request_builder.cpp`, the agent loop, or the TUI; the kilocode name-branch is gone; `ProviderCapabilities` is either consumed or deleted.

---

## 2. Audit Snapshot

### 2.1 What is already excellent (keep)

| Area | Evidence | Pattern |
|------|----------|---------|
| Agent port | `LLMClient` pure virtual `probe_server/chat/chat_stream` (`include/agent/llm.h:71-110`); `Agent` uses only the port via injectable `LLMClientFactory` (`include/agent/agent.h:50-51`); `FakeLLMClient` slots in for hermetic tests | Ports & Adapters, Factory |
| Provider domain | `Provider` pure data; `ProviderRepository`/`ModelCatalog` ports; service facade; static + file repos merged with later-wins override (`providers_service.cpp:32-57`) | Registry, Facade |
| Provider UX | `/provider list\|add\|edit\|delete\|test`, `/set provider`, feed leaves keyed per provider (`tui/feed_manager.cpp:29-41`), all tree/feed driven | JSON tree sole source |
| No name branching in core paths | Only `lib/model_probe.cpp:241` (`resolve_kilo_balance_token`) branches on `provider_name`; no `switch` on providers anywhere | - |
| Error tolerance where it is cheap | `parse_context_size_from_error` sniffing (`http_transport.cpp:56`), model-list shape tolerance (`model_probe.cpp:26-56`), `<think>` segmentation, XML tool-call text sniffing (`lib/tool_call_parser.cpp`) | Vendor-tolerant parsing, not vendor dispatch |

### 2.2 What blocks growth (verified file:line)

| ID | Sev | File:Line | Smell | Impact |
|----|-----|-----------|-------|--------|
| **A1** | High | `config.h:180-181` `api_url()/models_url()`; `http_transport.cpp:95` `apply_auth` (Bearer), `115` `message_from_completion`, `207` `curl_exec`, `56` `parse_context_size_from_error`, `160` `is_retryable_http_error`; `request_builder.cpp:75` `build_chat_body`; `sse_parser.cpp:146` `dispatch_event_impl`; `model_probe.cpp:59` `fetch_models` | OpenAI wire contract hardwired across 4 lib files + `Config` | Anthropic/Gemini-native requires editing every shared transport/parse file |
| **A2** | High | `providers.h:40-44` `ProviderCapabilities`; `providers_service.cpp:12-21` override table, **zero consumers** (grep-verified) | Declared extension point never threaded to the wire | Capability intent exists but cannot express behavior; next provider "needs" more bools (`supports_x`) → boolean explosion |
| **A3** | Medium | `model_probe.cpp:236-243` `resolve_kilo_balance_token` branches `provider_name == "kilocode"` | Name branch in core path | The one violation of "never branch on provider names"; will multiply per provider feature |
| **A4** | Medium | `providers_catalog_http.cpp:14-24` `HttpModelCatalog` shims `Provider → Config` then probes OpenAI `/models`; `Config` carries no dialect identity | Catalog hardwired to one probe format | `/provider test` and model listing can never serve a non-OpenAI provider |
| **A5** | Low | `config.h` `api_key` only; headers `http_transport.cpp:95` | Auth = Bearer xor nothing | Non-Bearer auth (Anthropic `x-api-key`, Gemini query key) impossible |
| **A6** | Low | `docs/spec/plugins/plugin-framework.md` §9 `ProviderImpl` capability, design-only; `Capability.impl` is `void*`; no `ProviderImpl` type exists | Plugin provider path aspirational | Fine as Phase-5 follow-up **if** the in-process seam (this proposal) matches the spec'd shape |

No dead legacy dispatch exists to delete; this is pure seam-insertion + one behavior proof.

### 2.3 Current wire-layer test coverage (the regression-net baseline)

Verified against `tests/run_tests.cpp` (the moving surface of FIX-028):

**Already pinned (will relocate with the code):**

| Moving function | Direct tests |
|---|---|
| `build_chat_body` | `request_body_survives_invalid_utf8`, `request_builder_merges_consecutive_system_messages`, `request_builder_hoists_midstream_system_into_leading_block`, `request_builder_assistant_message_always_has_content`, `request_builder_sanitizes_placeholder_tool_calls`, `request_builder_all_placeholder_tool_calls_degrades` + `llm_reasoning_effort_in_request_body` (e2e) |
| `message_from_completion` | `message_from_completion_sanitizes_tool_calls` |
| `is_retryable_http_error` | `http_error_empty_stream_400_is_retryable`, `http_error_json_400_is_not_retryable` (covers 400-empty/SSE-comment, JSON 400, 429, 5xx, 401/403) |
| SSE state machine | `llm_streaming_parser_accepts_auto_lambda_sink`, `llm_streaming_merges_tool_call_fragments`, `llm_streaming_tool_call_object_arguments_preserved`, `llm_streaming_inline_think_segmentation`, `llm_streaming_reasoning_content_field`, `llm_streaming_captures_usage_stats`, `sse_tool_call_index_capped`, `sse_raw_body_bounded`, `sse_one_based_tool_call_index_compacted`, `sse_id_only_tool_call_dropped` |
| `parse_models` / `parse_model_list_info` | `probe_parse_llamacpp_models`, `probe_parse_models_array_fallback`, `probe_parse_models_malformed_is_not_ok`, `probe_parse_kilocode_context_length`, `probe_prefers_n_ctx_over_context_length`, `probe_prefers_the_active_model`, `probe_active_model_without_meta_is_unknown`, `probe_falls_back_to_first_model_with_context`, `probe_parse_model_list_with_ctx`, `probe_parse_model_list_malformed`, `probe_parse_model_list_ollama_shape`, `parse_model_list_dedupes_ids` |
| `Config::api_url()` | asserted at `run_tests.cpp:53` |
| e2e through `HttpLLMClient` + mock SSE | `probe_autodetect_*` (×2), streaming chat e2e (ports 8911-8914), reasoning-effort e2e (8915), agent-factory e2e |

**Gaps on the moving surface (blind spots FIX-027 must close before the move):**

| Gap | Moving function | Why it matters |
|---|---|---|
| **G1** | `parse_context_size_from_error` pattern table (`http_transport.cpp:56-87`) | No direct test exists; the 400-prose learner is subtle (8 patterns + digit scan + sanity cap) and only fires on real 400s, a move could silently drop a pattern |
| **G2** | `apply_auth` (`http_transport.cpp:95-98`) | No direct test of header emission (Bearer with/without key); mock server ignores headers |
| **G3** | Buffered `chat()` path: `message_from_completion` degradation + `fill_buffered_stats` usage mapping | All e2e mock tests exercise `chat_stream`; the buffered path's malformed-response degradation ("[error: malformed LLM response…]") and `usage` → `Stats` mapping have no end-to-end pin |
| **G4** | `Config::models_url()` + learned-context propagation | `api_url()` is asserted but `models_url()` is not; `learned_context_size()` after a mock 400 overflow is never exercised |
| **G5** | Mid-stream cancellation (`CancelledError`) through `HttpLLMClient` | Cancel-token is unit-tested (`cancel_token_*`) and bash-tool e2e-tested, but not the LLM transport path |

**Status (2026-09-09):** G1-G5 are closed by the FIX-027 pin suite on `test/pin-wire-layer` (`tests/run_tests.cpp`, "Wire-layer characterization pins" section). Two pins revealed a design fact worth recording: the transport's 400-overflow "learned context" mutation was **not observable** through `HttpLLMClient::learned_context_size()` (capture happened only on the success path, but the learning only ever happens on the throwing 400 path), fixed by FIX-032 (§6).

---

## 3. Goals, Non-Goals, Design Decisions

### 3.1 Goals

- **Add an OpenAI-compatible provider** = config file (today), unchanged.
- **Add a genuinely different wire protocol** = one new `Dialect` implementation + one capability-table row (+ optional preset row). Zero edits to shared transport/parse/agent/TUI code.
- **Dispatch once, then polymorphic.** Flavor is resolved to a `Dialect` object exactly once per client construction; no code downstream ever compares provider names or flavors.
- **Capabilities become real or die.** `ProviderCapabilities` fields are consumed by the wiring (`apply_selection` → `Config`), and obsolete fields are deleted, not kept.
- **Composition over subclassing.** Not `AnthropicLLMClient : LLMClient` (N providers × features → class explosion); one transport orchestrator + pluggable dialect strategies.
- **Preserve behavior, proven, not assumed.** The OpenAI path after the refactor is byte-identical in behavior; the FIX-027 pin suite plus the untouched existing suite is the proof mechanism (§3.4).
- **Hermetic proof.** The second dialect is validated with pure body/parse/stream tests against recorded fixtures. No live provider calls in `make test`.

### 3.2 Non-Goals / Constraints

- No change to the agent loop, `AgentHooks`, `Context`, or the command tree.
- No TUI/CLI command-surface change (the tree already serves providers).
- No whole-app coverage campaign before the refactor (see D7, targeted pins replace it).
- No live-network Anthropic/Gemini testing in CI (opt-in env-gated manual check only).
- No plugin `Capability` type change now (`void*` stays; the dialect registry is the seam a future plugin `ProviderImpl` will register into, aligned shape, deferred wiring).
- No speculative second dialect beyond the one proof (Anthropic). Gemini etc. are exercises of the same seam, not new architecture.
- Keep `Config` a data struct: it carries the `flavor` **string**, never runtime dialect objects.

### 3.3 Design decisions (with rejected alternatives)

| # | Decision | Why | Rejected alternative |
|---|----------|-----|----------------------|
| **D1** | Introduce a `Dialect` port covering URL, auth, request body, buffered parse, streaming decode, model-list parse, error classification | These are exactly the seven things that differ between Anthropic/Gemini/OpenAI; grouping them makes a dialect a cohesive unit a reviewer can sign off in one file | Tiny per-concern interfaces (`IBodyBuilder`, `IAuth`), assembly overhead, and most variance is *correlated* per protocol |
| **D2** | `flavor` string in `Config`, set by `apply_selection` from the capability table; `HttpLLMClient` resolves flavor → dialect via a registry | Keeps `Config` serializable/data; single indirection point; user provider files default to `"openai"` | Putting `shared_ptr<Dialect>` in `Config` (breaks the data struct, complicates copies/persistence); branching on flavor strings at call sites (explosion, forbidden) |
| **D3** | Dialects normalize to the **existing internal DTOs** (`Message`, `StreamChunk`, `Stats`) at the edge; Anthropic tool calls are synthesized into the internal `{id,type:"function",function:{name,arguments}}` shape | The agent loop, history, and tool dispatch stay provider-agnostic, that is the whole point of the port | New internal DTO per provider (cascades through Context, dispatch, session restore) |
| **D4** | Registry = flavor → factory map with built-ins in one OCP-open registration function; plugins register into the same map later | One extension point, runtime-extensible later, no `switch` | `switch(flavor)` in the client (the explosion we are avoiding) |
| **D5** | Replace `ProviderCapabilities{bearer_auth, supports_reasoning_effort, flavor}` with `{flavor, api_key_is_account_token}`; auth and reasoning-effort behavior move into the dialect where they belong | `bearer_auth`/`supports_reasoning_effort` are bools that *simulate* what the dialect *does*; keeping them invites `supports_x` boolean explosion. The dialect expresses auth and body fields structurally. The kilocode balance-token quirk is not wire behavior, it is an amber feature keyed off the provider's key semantics, so it stays a capability **bool**, not a dialect method | Growing the bool struct; keeping dead fields |
| **D6** | `StreamParser` becomes an abstract incremental decoder (`StreamDecoder`); the current OpenAI state machine moves into the default dialect's decoder; `http_transport` feeds bytes to the interface | Anthropic streams named events (`content_block_delta`…), a different state machine that cannot be expressed as an OpenAI-event tweak. Framing (line splitting, raw capture) is shared and stays in the base | `if (flavor)` inside `StreamParser`; forking the whole transport |

### 3.4 Regression-protection strategy (how the refactor cannot break the app)

| # | Decision | Why | Rejected alternative |
|---|----------|-----|----------------------|
| **D7** | **Characterization pins on the moving surface, not full coverage.** Before any code moves (FIX-027), close the §2.3 gap list (G1-G5) with tests written against current symbols. They pass today, they *define* current behavior (Feathers-style characterization). Whole-app coverage would spend most of its budget on code the refactor never touches | The blast radius of FIX-028 is precisely the listed functions; pins + the existing 40+ wire tests fully bound it. Full coverage of the TUI/agent loop adds assurance the move doesn't need, at real cost | "Full coverage first", wrong ROI; most uncovered code is untouched by the refactor |
| **D8** | **Move code and its tests together, with an "assertions unchanged" gate.** FIX-028 lands as move-only commits (`git diff -M` shows renames; reviewers diff the test hunks and require that every `ASSERT_*` line is byte-identical, only the call path/namespace changed) | Pins written against today's free functions must follow those functions into the dialect. Assertion equality across the move is the verifiable proof of behavior preservation, stronger than trusting "I didn't change logic" | Keeping shim wrappers so tests don't move (violates the repo's no-wrapper rule); rewriting pins against the future API before the move (cannot compile) |
| **D9** | **Mutation spot-check after the move.** One uncommitted, deliberate behavior break inside the new openai dialect (e.g. drop one `parse_context_size_from_error` pattern); confirm the pin suite fails; revert. Repeat for 2-3 representative pins (body, stream, retry) | Proves the net bites, that the pins would catch a real regression, not merely compile | Trusting green pins without ever seeing one fail |
| **D10** | **Keep every existing gate.** Untouched behavioral suites (`run_tests`, `command_line_test`, `e2e_test`, `session_browser_test`, `completions_test`) stay green per commit; `make lint`/`make analyze` zero-new-findings per commit; g++/clang++ matrix in CI; PR checklist adds a **manual smoke** (CLI + TUI one-turn chat against the developer's own OpenAI-compatible endpoint) before merge | Cheap, layered: unit pins → integration mocks → static analysis → manual smoke | - |

Note on Red→Green: FIX-027 is **green by construction** (characterization tests pass on day one, that is their purpose). The Red→Green discipline applies from FIX-028 onward, where each new behavior (registry fallback, capability copy, anthropic dialect) starts as a failing test.

---

## 4. Target Architecture

### 4.1 Layering (hexagonal, unchanged, enforced)

```
Domain core  lib/ + include/agent/
   ports: Tool, SearchBackend, LLMClient, AgentHooks, ProviderRepository, ModelCatalog,
          Dialect (NEW), StreamDecoder (NEW, extracted)
   use-case: Agent (depends on LLMClient only)
   wiring:   make_default_provider_service, make_dialect(flavor), LLMClientFactory
Adapters
   wire:   dialect_openai (moved from today's request_builder/http_transport/sse_parser/model_probe),
           dialect_anthropic (NEW, proof)
   data:   providers_repo_static/files, providers_catalog_http (now dialect-driven)
Hosts    src/amber-cli, tui/, unchanged command surface; apply_selection picks up flavor automatically
```

`lib/` never includes `tui/`/`src/`; hosts never touch dialect internals.

### 4.2 The Dialect port (new `include/agent/dialect.h`)

```cpp
namespace agent {

// Port: everything that differs between LLM provider wire protocols.
// One implementation per flavor ("openai", "anthropic", ...). Resolved once
// per client construction; callers never branch on flavor or provider names.
class Dialect {
public:
    virtual ~Dialect() = default;

    virtual std::string flavor() const noexcept = 0;          // "openai", ...

    // Endpoints. models_url() == "" means the provider has no model listing.
    virtual std::string chat_url(const Config&) const = 0;
    virtual std::string models_url(const Config&) const = 0;  // "" = unsupported

    // Auth: anything beyond the Bearer default (x-api-key, query key, none).
    virtual void apply_auth(HeaderList&, const Config&) const = 0;

    // Request body + buffered response parse. Both pure: unit-testable
    // without libcurl.
    virtual json build_chat_body(const Config&, const std::vector<Message>&,
                                 const std::vector<std::shared_ptr<Tool>>&,
                                 bool stream) const = 0;
    virtual Message parse_completion(const std::string& raw) const = 0;

    // Streaming: the dialect owns the event state machine. The shared
    // framing (line splitting, raw capture, cancellation) lives in the
    // StreamDecoder base.
    virtual std::unique_ptr<StreamDecoder> make_decoder(
        Message& out, const StreamDecoder::ChunkSink& on_chunk,
        std::string debug_path) const = 0;

    // Model listing parse (pure).
    virtual std::vector<ModelInfo> parse_models_response(
        const std::string& raw) const = 0;

    // Error classification: retry policy + context-overflow learning.
    virtual bool is_retryable(long http_code, const std::string& body) const = 0;
    virtual int  context_overflow_hint(const std::string& error_body) const = 0;
};

// Registry (D4). Built-ins register here; unknown flavors fall back to
// "openai" with a debug log (user provider files always default to openai).
std::unique_ptr<Dialect> make_dialect(const std::string& flavor);
void register_dialect(const std::string& flavor,
                      std::function<std::unique_ptr<Dialect>()> factory);

} // namespace agent
```

`HttpLLMClient` gains an optional dialect parameter (default: `make_dialect(cfg.flavor)`), so the `LLMClientFactory` signature, and therefore `Agent`, is untouched:

```cpp
// llm.h, existing port unchanged; production impl grows a seam:
class HttpLLMClient : public LLMClient {
    explicit HttpLLMClient(Config cfg, std::unique_ptr<Dialect> dialect = nullptr);
    // ...
};
```

### 4.3 Flavor plumbing (D2, D5)

`Config` gains two derived-from-selection fields (not persisted; re-derived from the provider on every boot, exactly like `api_base` today):

```cpp
std::string flavor = "openai";                 // wire dialect selector
bool api_key_is_account_token = false;         // kilocode: api_key powers balance readout
```

`apply_selection` (`lib/providers_service.cpp:114`) copies them from the resolved capability:

```cpp
void apply_selection(Config& cfg, const ProviderSelection& sel) {
    cfg.provider_name = sel.provider.name;
    // ... existing api_base/api_key/model/context copies ...
    const ProviderCapabilities caps =
        capabilities_of(sel.provider.name);            // shared helper
    cfg.flavor = caps.flavor;
    cfg.api_key_is_account_token = caps.api_key_is_account_token;
}
```

`ProviderCapabilities` shrinks to fields that are genuinely data, not simulated behavior:

```cpp
struct ProviderCapabilities {
    std::string flavor = "openai";             // dialect selector (D1/D5)
    bool api_key_is_account_token = false;     // amber-feature quirk (balance readout)
};
```

The kilocode balance branch (`model_probe.cpp:241`) becomes capability-driven and name-free:

```cpp
std::string resolve_balance_token(const Config& cfg) {
    if (!cfg.kilo_balance_token.empty()) return cfg.kilo_balance_token;
    if (cfg.api_key_is_account_token) return cfg.api_key;   // no name compare
    return "";
}
```

### 4.4 Default dialect = pure move of today's OpenAI behavior

| Today (hardwired) | Becomes (openai dialect) |
|---|---|
| `Config::api_url()/models_url()` (`config.h:180`) | `chat_url/models_url`, methods deleted from `Config` |
| `apply_auth` (`http_transport.cpp:95`) | `Dialect::apply_auth`, Bearer default |
| `build_chat_body` + `sanitize_*` (`request_builder.cpp`) | `Dialect::build_chat_body`, moved file `lib/dialect_openai.cpp`; `request_builder.h` deleted after consumers re-home |
| `message_from_completion` (`http_transport.cpp:115`) | `Dialect::parse_completion` |
| `StreamParser` state machine (`sse_parser.cpp:146-211`) | `OpenAIStreamDecoder : StreamDecoder` (framing in the base) |
| `parse_models`/`parse_model_list_info` (`model_probe.cpp:85-180`) | `Dialect::parse_models_response` |
| `parse_context_size_from_error`/`is_retryable_http_error` (`http_transport.cpp:56,160`) | `Dialect::context_overflow_hint` / `is_retryable` |

`http_transport.cpp` shrinks to curl mechanics: URL/auth/body from the dialect, bytes into a `StreamDecoder*`, status learning via `dialect->context_overflow_hint`. `model_probe.cpp` keeps the two kilo.ai **amber-feature** calls (`fetch_kilo_balance`, `resolve_balance_token`) which are product features, not wire dialects. `model_probe.h`'s `probe_server/apply_server_autodetect/merge_server_info` stay as core services delegating to the client.

`HttpModelCatalog` (`providers_catalog_http.cpp`) becomes dialect-driven: build a `Config` from the `Provider`, resolve `make_dialect(caps.flavor)`, GET `dialect->models_url()` with `dialect->apply_auth`, parse with `dialect->parse_models_response`. Empty `models_url` ⇒ empty list ⇒ `/provider test` reports "no model listing" instead of a false negative.

### 4.5 File map (target state)

```
include/agent/dialect.h            NEW   ~70 lines  Dialect port + registry decls
include/agent/stream_decoder.h     NEW   ~60 lines  abstract decoder (framing base)
lib/dialect.cpp                    NEW   ~60 lines  registry + openai fallback
lib/dialect_openai.cpp             NEW   ~260 lines body/parse/error/decode (pure moves
                                                 + request_builder + sse openai state machine)
lib/dialect_anthropic.cpp          NEW   ~200 lines proof dialect (hermetic)
lib/stream_decoder.cpp             NEW   ~80 lines  framing shared by decoders
lib/http_transport.cpp             ~330→~120 lines curl only (URL/auth via dialect)
lib/model_probe.cpp                ~245→~150 lines probe via client/dialect; kilo features stay
include/agent/sse_parser.h         DELETE (→ stream_decoder.h + openai decoder)
include/agent/request_builder.h    DELETE (→ dialect.h; sanitizers re-home: wire-time
                                                 ones into dialect_openai, history-hygiene
                                                 ones stay in core, decide per consumer)
```

Size limits: no class >200 lines, no method >10 with minimal branching, the openai dialect file is free-function-heavy like today's files (method-implementation exempt per audit convention).

---

## 5. Phased Execution (Red → Sign-off → Green per FIX)

| Phase | FIX ID | Scope | Depends | Size | Gate |
|-------|--------|-------|---------|------|------|
| **0** | `FIX-027` | **Pin the wire layer**: close §2.3 gaps G1-G5 with characterization tests against current symbols; optional local `gcov` pass over the moving functions to confirm no further blind spots | - | M | Pin suite green on *current* code; gap list closed; no production-code changes in the diff |
| **1** | `FIX-028` | Dialect seam: registry + `Dialect` port + `StreamDecoder` extraction; OpenAI behavior moved in **pure moves**; `HttpLLMClient` resolves dialect from `cfg.flavor`; pins relocate with their code | 027 | L | Pin suite green with **only mechanical renames** (D8 gate); untouched suites green; `grep` proof: no `api_url(` outside dialects; mutation spot-check (D9) passes |
| **2** | `FIX-029` | Capabilities reach the wire: `Config.flavor`/`api_key_is_account_token`; `apply_selection` copies; dead `bearer_auth`/`supports_reasoning_effort` deleted; kilocode name-branch removed | 028 | S | Red test for name-free balance token; `grep provider_name ==` → 0 in `lib/` |
| **3** | `FIX-030` | Proof dialect: Anthropic Messages API (auth, URL, body, buffered parse, event-stream decode, models parse), hermetic fixtures only; static preset `anthropic` + capability row | 028, 029 | L | New `tests/dialect_test.cpp` green with zero live calls; zero edits to `http_transport.cpp`/agent/TUI |
| **3.5** | `FIX-032` | Fix the learned-context capture: the 400-overflow window is learned on the throwing path; surface it through `learned_context_size()` on both exits so `Agent::resolve_window()` can clamp | 028 | S | The three FIX-027 pins flip from `0` to the learned values (red first); `agent_loop_learned_window_clamps_gate` stays green |
| **4** | `FIX-031` | Docs & alignment: new `docs/spec/llm-client/dialect.md`; update `http-transport/model-probe/streaming.md` to dialect terms; note plugin v2 Phase-5 `ProviderImpl` = dialect factory registration; refresh `AGENTS.md` audit if line counts moved | 028-030 | S | Specs consistent with code (code is truth); no dead prose |

**Order rationale:** 027 pins the blast radius of the whole effort *before* any production line moves, it is the insurance the user asked for, and it is cheap (test-only, green by construction). 028 is a behavior-preserving move with the biggest diff but zero risk once 027 is in (pins + existing tests are the net) and unblocks everything; 029 is tiny and independent enough to ride along after; 030 is the acceptance proof and must land only on top of a green 028+029 so any breakage is attributable to the new dialect, not the refactor; 031 is docs.

### Branch & PR discipline

- One branch per FIX off `main`: `test/pin-wire-layer` (027), `refactor/provider-dialect-seam` (028), `refactor/provider-capabilities-wired` (029), `feat/provider-dialect-anthropic` (030), `docs/provider-dialect-spec` (031). Squash-merge, imperative scoped messages (`refactor: extract wire dialect port from HttpLLMClient`).
- FIX-028 first commit = **pure move** (no logic change); second commit = registry/decoder extraction with new tests. Reviewable as "move" then "seam".
- Red commits land first on each branch from FIX-029 onward so CI shows the failure, per AGENTS.md (027 is green-by-construction characterization; 028's pins are relocated, not rewritten).
- Reviewer checklist per AGENTS.md: SOLID, hexagonal, ≤200/≤10, Red→Green, hermetic, zero new clang-tidy/cppcheck findings, no dead code. **Extra for 028:** test-hunk diff shows `ASSERT_*` lines byte-identical (D8).

---

## 6. Detailed Designs

### FIX-027, Pin the wire layer (characterization, no production changes)

Implemented on `test/pin-wire-layer` (2026-09-09). New `TEST` blocks in `tests/run_tests.cpp` under a "Wire-layer characterization pins (FIX-027)" section, plus two test-only mock helpers (`spawn_mock_http`, `spawn_stall_server`; `spawn_mock_sse` refactored to delegate, behavior unchanged, existing call sites untouched):

- **G1 → `overflow_400_streaming_throws_non_retryable_api_error` + `overflow_400_buffered_throws_non_retryable_api_error`.** One e2e mock per transport path (llama.cpp-style plain-text 400 on the stream path; OpenAI-style JSON-wrapped 400 on the buffered path). Pins: typed `ApiError` with `status == 400`, `retryable == false`, message prefixed `"HTTP 400 from LLM server"`, never a fabricated reply, never a retry. Per-pattern assertions on the prose sniffer (`parse_context_size_from_error`, `http_transport.cpp:56-87`) are **not observable** at the client level (see note below), so they move to FIX-028, where `Dialect::context_overflow_hint` is a public virtual and each pattern gets a direct assertion.
- **G2 → `apply_auth_emits_bearer_only_with_key`.** Direct `apply_auth` pin: no key → no header; key → exactly one `Authorization: Bearer <key>` header.
- **G3 → `buffered_chat_fills_stats_from_usage` + `buffered_chat_malformed_body_degrades_to_recovery_message`.** Buffered e2e: `usage.prompt_tokens/completion_tokens` land in `Stats` (`valid`, token counts, latency); a 200 with a non-JSON body degrades to the `"[error: malformed LLM response, raw body follows]"` assistant message with the raw body preserved, never an exception.
- **G4 → `config_models_url_derivation` + `client_serves_next_turn_after_overflow_rejection`.** `models_url()` derivation asserted; and (stronger than the original plan) the same client instance survives a 400 rejection and serves a healthy next turn, the rejection's internal `cfg_` mutation (`context_size` learned, `context_explicit` set) must not poison later requests.
- **G5 → `llm_cancel_pre_requested_aborts_with_cancelled_error` + `llm_cancel_mid_stream_aborts_with_cancelled_error`.** A token requested before the call aborts fast; a mid-flight cancel against a stalled server aborts via curl's progress callback (~1s) with typed `CancelledError`, distinct from `ApiError`.

**Note, learned-context capture gap (was the FIX-027 characterization finding, now fixed by FIX-032):** `post_completion`/`stream_completion` parse the 400 prose and mutate the client's internal `cfg_.context_size` **before throwing**, but `HttpLLMClient` captured `learned_` only after a successful return, and a 400 always throws. `learned_context_size()` was therefore always 0 in production, so `Agent::resolve_window()`'s "server taught us via a 400 rejection" clamp (`lib/agent.cpp:98-103`) could never fire from the transport path. The FIX-027 pins documented this fact (`learned_context_size() == 0` after a 400) so the pure-move refactor preserved it exactly; FIX-032 (§6) then flipped those assertions to the corrected behavior and fixed the capture.

Gate: pin suite green on current code (verified: full core suite 431/431 on `test/pin-wire-layer`, TUI section excluded on macOS by a pre-existing ncurses `BUTTON5_PRESSED` gap in `tui/scroll_dispatch.cpp`/`tui_tests.cpp` unrelated to this change); diff contains **zero production-code changes**; `make format-check` reports no violations in `tests/run_tests.cpp`.

### FIX-028, Dialect seam (pure move first)

**Implemented** on `refactor/provider-dialect-seam` (2026-09-09, stacked on FIX-027). What landed:

**New files**: `include/agent/dialect.h` (the port + `TokenUsage` + registry), `include/agent/dialect_openai.h` (openai dialect factory + the `sanitize_*` wire helpers), `lib/dialect.cpp` (flavor→factory table, unknown flavor falls back to `openai`), `lib/dialect_openai.cpp` (all moved OpenAI behavior: URLs, auth, body builder, buffered parse, usage, model-list parse, error classification/sniffing, `OpenAIStreamDecoder`), plus `include/agent/stream_decoder.h` + `lib/stream_decoder.cpp` (framing base).

**Moved verbatim into the dialect**: `build_chat_body` plus the system-merge/tool schema logic (`request_builder.cpp`), `message_from_completion`, `is_retryable_http_error`, `parse_context_size_from_error`, `read_usage_token` (`http_transport.cpp`), `parse_entry`/`model_array`/`parse_models`/`parse_model_list_info` (`model_probe.cpp`), and the `StreamParser` event state machine (`sse_parser.cpp`).

**Deleted**: `include/agent/sse_parser.h`, `lib/sse_parser.cpp`, `include/agent/request_builder.h`, `lib/request_builder.cpp`.

**Rewired**: `HttpLLMClient(Config)` / `HttpLLMClient(Config, unique_ptr<Dialect>)`: the client resolves `make_dialect(cfg.flavor)` and drives URL/auth/body/parse/decoder/usage through it; `http_transport` is curl mechanics only (`curl_exec` takes the dialect for URL + auth, `post/stream_completion` use `context_overflow_hint`/`is_retryable`, `fill_buffered_stats` uses `parse_usage`); `model_probe` fetches with the dialect's endpoint/auth and parses with `parse_models_response`/`parse_model_list_response`, keeping the kilo.ai balance features and the config-based public API (`probe_server(cfg)`, `list_model_info(cfg)`, `list_models(cfg)`) unchanged for TUI/agent consumers; `Config` gains `flavor` and loses `api_url()`/`models_url()`.

**Deviations from the plan (documented per D8/D9 discipline):**

1. **`auth_headers()` instead of `apply_auth(HeaderList&, …)`.** The port must not carry curl types, so a dialect returns `std::vector<std::string>` and the transport adapts. The FIX-027 `apply_auth_emits_bearer_only_with_key` pin was updated to drive the port: **the asserted values are identical** (no key → no header; key → exactly one `Authorization: Bearer …`), only the call expression changed. Same for the two URL pins (`chat_url`/`models_url` replace `Config::api_url()/models_url()`), which assert the same strings.
2. **`parse_usage` added to the port.** Buffered stats must not be OpenAI-keyed (`usage.prompt_tokens` vs Anthropic's `usage.input_tokens`); without it FIX-030 would have to modify the transport.
3. **`make_decoder` through the port, not a free factory.** The decoder is wire behavior; tests and `bench/probe.cpp` construct it via `make_dialect("openai")->make_decoder(...)`.
4. **New pin added:** `dialect_context_overflow_hint_patterns`, the per-pattern assertions promised in the FIX-027 note, now that the sniffer is a public virtual (8 pattern families + JSON-wrapped + no-match + sanity cap).
5. **Test names `request_builder_*` kept.** They name the behavior (request body assembly), not the deleted file; renaming would churn the pins for cosmetics.

**Mutation spot-checks performed (D9):** (a) renamed one `context_overflow_hint` pattern → `dialect_context_overflow_hint_patterns` FAILED; (b) perturbed the system-merge in `append_messages` → `request_builder_merges_consecutive_system_messages` FAILED. Both reverted. The net bites.

Gate: full core suite **432/432** on the final state; `cli` and `bench` binaries build; `make check` invariants hold; `make format-check` clean for touched files; `grep -rn 'api_url()\|models_url()' lib/ include/ tui/ src/` → 0; `grep -rn 'request_builder\|sse_parser' lib/ include/` → 0 (only historical test names remain).

### FIX-029, Capabilities reach the wire

**Implemented** on `refactor/provider-dialect-seam` (2026-09-09, same branch as FIX-028).

**Red first:** `apply_selection_copies_flavor_and_account_token_flag`, `flavor_and_capability_flag_not_persisted`, and the extended `resolve_kilo_balance_token_falls_back_to_kilocode_api_key` (name-only config now yields nothing; a flagged config yields the key) were written and confirmed failing to compile before the implementation landed.

**Green:** `Config` gains `api_key_is_account_token` (`flavor` arrived with FIX-028); `ProviderCapabilities` shrinks to `{flavor, api_key_is_account_token}`, the simulated-behavior bools (`bearer_auth`, `supports_reasoning_effort`) are **deleted**, not kept; the override table declares `kilocode` as account-token-backed and both built-ins as `openai`; a single file-local `capabilities_of(name)` helper is the one source both `apply_selection` and any future consumer consult; `apply_selection` copies both fields (derived on every selection, never persisted); `resolve_kilo_balance_token` decides on the capability flag instead of `provider_name == "kilocode"`.

**Deviations from the plan:**

1. **`ProviderService::capabilities()` deleted instead of delegating.** The free helper is the single source; the public accessor had zero callers, so keeping it would have been dead API (`AGENTS.md`: no dead code). If a future caller needs capabilities by name, it re-consults the same helper.
2. **`resolve_kilo_balance_token` keeps its name.** The readout IS kilo.ai-specific (`fetch_kilo_balance` hardcodes the kilo endpoint); only the provider-name branch was the violation. Renaming would have churned the TUI for cosmetics, so the name stays and the branch is gone.

Gate: `grep -rn 'provider_name ==' lib/ include/` → 0 (the three remaining `provider_name ==` in `tui/tui_input.cpp` compare the *active* provider for display/identity, not behavior); `bearer_auth`/`supports_reasoning_effort` → 0 mentions; full suite **434/434**; cli + bench build; `make check` holds.

### FIX-030, Anthropic proof dialect (hermetic)

**Implemented** on `refactor/provider-dialect-seam` (2026-09-09, same branch as FIX-028/029). The acceptance proof for the whole proposal.

**Footprint, exactly what the seam promised:**

| Change | File |
|---|---|
| New dialect (protocol + event decoder) | `lib/dialect_anthropic.cpp`, `include/agent/dialect_anthropic.h` |
| Registry row | `lib/dialect.cpp` (one table entry) |
| Provider preset row | `lib/providers_repo_static.cpp` (`anthropic`, `https://api.anthropic.com`) |
| Capability row | `lib/providers_service.cpp` (`{"anthropic", {"anthropic", false}}`) |
| Build + tests | `Makefile.in`, `tests/dialect_anthropic_test.cpp` |

**Zero changes** to `lib/http_transport.cpp`, `lib/llm.cpp`, `lib/model_probe.cpp`, `include/agent/agent.h`, the agent loop, the command tree, or the TUI. That is the growth claim, demonstrated rather than asserted.

**What the dialect implements**: Messages API translation: `chat_url` = `/v1/messages`; `auth_headers` = `x-api-key` + `anthropic-version`; one merged top-level `system` string; internal messages mapped to content blocks (`text`, `tool_use` with parsed `input`, `tool_result` with `tool_use_id`); tools carry `input_schema` (no `type:"function"` discriminator); `parse_completion` maps `content[]` blocks back to text + the internal tool_calls shape; `make_decoder` is an event-type state machine (`message_start` → usage, `content_block_start/delta/stop`, `input_json_delta` accumulation, `message_delta` → output tokens, `message_stop`), dropping nameless tool blocks at end like the OpenAI decoder; model listing from `/v1/models` (no context window reported, stays 0, never fabricated); `parse_usage` maps `input_tokens`/`output_tokens`; retry classification covers 429/5xx **including Anthropic's 529 overloaded**; the overflow hint parses `"… N tokens > M maximum"`.

**Known limits (documented in the file, not silently dropped):** `cfg.thinking`/`reasoning_effort` have no Messages API equivalent and are not sent; streaming thinking deltas are collected when the API emits them.

**Tests (all hermetic, no network):** `tests/dialect_anthropic_test.cpp`, 12 tests covering endpoints, auth headers, full request translation (merged system, tool_use/tool_result blocks, parsed input), `input_schema` tools, buffered parse + usage, malformed-body degradation, the named-event stream decode (text + streamed JSON tool arguments + usage), nameless-tool-block dropping, model list/probe parse, error classification/overflow prose, plus the two registry tests (resolve, fallback, and registering a new factory).

Gate: full suite **446/446**; cli + bench build; `make check` holds; cppcheck adds no findings.

### FIX-032, The learned context window actually surfaces

**Implemented** on `refactor/provider-dialect-seam` (2026-09-09, same branch).

**The bug:** `post_completion`/`stream_completion` learn the server's true window from a 400 overflow rejection by mutating the `Config` they were handed, and then throw. `HttpLLMClient` captured `learned_` only *after* a successful return, so the only code that ever mutates the window was unreachable from the capture. `learned_context_size()` was always 0 in production and `Agent::resolve_window()`'s clamp never fired.

**Red first:** `2af136a`, the three FIX-027 pins that documented the gap now assert the corrected behavior (streaming `2048`, buffered `16384`, sticky `8192` across a healthy turn) and fail with `0 != expected`.

**The fix:** a 15-line scope guard (`LearnedWindowCapture`) in `lib/llm.cpp` captures the window as the scope exits, success **or** failure. This covers both transport paths (`chat`, `chat_stream`) with one mechanism and no duplicated catch blocks. Chosen over (a) try/catch duplication in two call sites and (b) threading the value through `ApiError` (changes the error contract; more surface than the fix needs).

**Behavior change (intended):** a server that rejects an oversized request now actively clamps the gauge and the compression budget to what it enforces, the contract the code comments and `agent_loop_learned_window_clamps_gate` already described. That agent-level test (which uses a fake returning a learned window) stays green; the real client now delivers what the fake simulated.

Gate: the three pins green; full suite **446/446**; cli + bench build; `make check` holds.

### FIX-031, Docs & plugin alignment

**Implemented** on `refactor/provider-dialect-seam` (2026-09-09, same branch): `dialect.md` written; `http-transport` / `streaming` / `model-probe` / `INDEX` / `plugin-framework` re-aligned with the code.

- New `docs/spec/llm-client/dialect.md` following `docs/spec/TEMPLATE.md` (Purpose/Ownership/Contract/Scenarios `SC-##`/Cross-references), the spec's "if code and spec disagree, the code is wrong" rule applying to the dialect port.
- Rewrite `http-transport.md`, `model-probe.md`, `streaming.md` to describe the openai dialect as *one implementation* of the port (they already document the default behavior precisely, mostly terminology + ownership lines).
- `docs/spec/plugins/plugin-framework.md` Phase 5 note: `ProviderImpl` maps to "register a dialect factory into `lib/dialect.cpp`'s registry", the in-process seam is the plugin target; no `Capability` type change now.
- Refresh `AGENTS.md` architecture-audit table if `wc -l` drifted on tracked files (`http_transport.cpp`, `model_probe.cpp`, `run_tests.cpp`).

---

## 7. Verification & KPIs

Per-PR:

```
make clean && make && make test && make lint && make analyze && make check
```

(g++/clang++ matrix; zero new clang-tidy/cppcheck findings.)

Per-FIX extra:

- FIX-027: pin suite green on current code; `git diff --stat` on the branch shows **test files only**; gap list G1-G5 closed (each maps to a committed TEST block).
- FIX-028: test-hunk review gate (D8), `ASSERT_*` lines byte-identical pre/post move; `grep -rn 'api_url()\|models_url()' lib/ include/ tui/ src/` → 0 outside dialect files; `StreamParser` symbol gone (replaced by `StreamDecoder`/`OpenAIStreamDecoder`); mutation spot-check (D9) demonstrated.
- FIX-029: `grep -rn 'provider_name ==\|bearer_auth\|supports_reasoning_effort' lib/ include/` → 0.
- FIX-030: `git diff --stat main...` shows zero changes in `lib/http_transport.cpp`, `lib/model_probe.cpp`, `include/agent/agent.h`, `tui/` (rename call site excepted); anthropic tests run with no network (test env has no keys; fixtures are inline strings).
- FIX-031: spec↔code cross-reference check; `make check` P5 audit table still accurate.

Manual (PR checklist, D10): one CLI and one TUI one-turn chat against the developer's own OpenAI-compatible endpoint (never the inviolate `llama-turboq:8081` service configuration, and never required in CI). Optional live Anthropic check, env-gated `AMBER_TEST_LIVE_ANTHROPIC=1`.

---

## 8. Best Practices Enforced by This Proposal

- **Strategy + Factory + Registry, dispatch-once**: the flavor string is the only switch; everything downstream is virtual dispatch. New provider = register, never edit shared code (OCP).
- **Composition over inheritance**: dialect strategies inside one `HttpLLMClient` orchestrator; no provider subclass tree (LSP held by every `Dialect` being substitutable).
- **KISS**: the seam is one interface and one registry, no micro-interfaces, no `std::variant` capability types (deferred, per prior YAGNI sign-off).
- **DRY**: shared SSE framing in the decoder base; capability table is the single source of provider behavior data; `capabilities_of` helper shared by service + `apply_selection`.
- **Zero dead code**: `ProviderCapabilities` fields either get consumers (flavor, account-token flag) or are deleted (bearer_auth, supports_reasoning_effort); `request_builder.h`/`sse_parser.h` are deleted, not kept as shims.
- **Boy Scout**: `http_transport.cpp` ~330 → ~120 lines; `model_probe.cpp` loses its name-branch; `Config` loses URL derivation (methods that made the data struct know wire details).
- **Characterization over coverage-chasing (D7)**: the regression net is *targeted* at the refactor's blast radius, pins written before the move, asserting current behavior, relocated with the code under an "assertions unchanged" review gate (D8), and proven to bite via mutation spot-checks (D9).
- **Hermetic TDD**: every behavior change from FIX-029 onward starts with a failing pure test; dialect bodies/parses/decoders are all testable without libcurl or network; ≥80% coverage on new paths.
- **Hexagonal**: dialect port lives in the core with the other ports; adapters (openai/anthropic) depend inward; hosts wire via `make_dialect`/`LLMClientFactory`.

---

## 9. Risks & Mitigations

| Risk | Mitigation |
|------|------------|
| Pure-move refactor (028) subtly changes wire behavior | FIX-027 pins close every identified blind spot first; move commits are mechanical with an "assertions unchanged" review gate (D8); untouched suites (`run_tests`/`command_line_test`/`e2e_test`) stay green per commit; mutation spot-check (D9) proves the pins bite |
| Characterization pins written against current symbols churn when symbols move | Pins relocate *with* the code in the same commit; the gate is assertion equality, not symbol stability, the compiler plus the review diff enforce it (D8) |
| "Pin everything" balloons into a coverage campaign | D7 scopes pins to the §2.3 gap list on the moving surface; existing 40+ wire tests are the baseline, not rewritten; gcov is an optional local sanity pass, not a committed gate |
| `sanitize_tool_calls`/`sanitize_tool_schema` have core (history-hygiene) consumers that must not become dialect-private | Grep all consumers during 028 before moving; wire-time sanitize goes with the dialect, restore-time hygiene stays core; no silent duplication |
| Anthropic streaming delta shapes drift from the fixture (field renames) | Fixtures recorded from the current Messages API; opt-in live test exists for humans with keys; parse is defensive (`str_or_raw`-style) like today |
| `Config` grows two fields, persistence/serialization code must not leak them | Fields are derived-on-boot, never written by `save_global`/`save_settings`; FIX-029 adds a red test asserting flavor is not persisted |
| Plugin v2 Phase 5 expects a different seam shape later | The registry is a plain flavor→factory map; a future `ProviderImpl` capability registers into exactly that map, no re-architecture (documented in 031) |
| Kilocode balance readout regresses during the rename | Red test first (`resolve_balance_token` with flag set returns `api_key`); TUI call site updated in the same commit as the rename |
| Pin tests pass but would not catch a regression (false confidence) | D9 mutation spot-check: deliberately break 2-3 representative behaviors post-move and confirm the suite fails; revert |

---

## 10. Success Criteria

- FIX-027 pin suite committed and green on current code with a test-only diff.
- `make clean && make && make test && make lint && make analyze` green on g++/clang++, `make check` invariants hold, on every FIX branch and after squash-merge.
- Adding the Anthropic provider touched: `lib/dialect_anthropic.cpp`, `lib/providers_repo_static.cpp` (preset row), `lib/providers_service.cpp` (capability row), `tests/dialect_anthropic_test.cpp`, and nothing else.
- `grep -rn 'provider_name ==' lib/ include/` → 0; `ProviderCapabilities` has two consumed fields; `request_builder.h` and `sse_parser.h` deleted.
- A future "Gemini dialect" is an exercise: same seam, no design change. That is the definition of unlimited growth without if/switch chaos.
- `docs/spec/llm-client/dialect.md` published and consistent with code; plugin-framework.md notes the registry as the Phase-5 target.

---

## 11. Appendix, File:Line Index for Reviewers

```
tests/run_tests.cpp:53                      api_url() assertion (G4 sibling)
tests/run_tests.cpp:298                     resolve_kilo_balance_token fallback test (029 rename target)
tests/run_tests.cpp:402-575                 build_chat_body + message_from_completion pins
tests/run_tests.cpp:1182-1308               probe parse pins (12 tests)
tests/run_tests.cpp:1308-1349               http error/retry pins
tests/run_tests.cpp:1457+                   spawn_mock_sse helper (e2e fixture server)
tests/run_tests.cpp:1522-1736               probe autodetect + streaming e2e (ports 8911-8914)
tests/run_tests.cpp:4845-5100               reasoning-effort e2e, sse edge-case pins
tests/run_tests.cpp:1618-1736,5031-5100     SSE pins that relocate into OpenAIStreamDecoder (028)
include/agent/llm.h:51,71-131               LLMClient port + HttpLLMClient (seam target)
include/agent/agent.h:50-51                 LLMClientFactory (unchanged)
include/agent/providers.h:27-44             Provider + ProviderCapabilities (shrink, D5)
include/agent/providers.h:51-65,78-121      ProviderRepository/ModelCatalog/ProviderService
include/agent/providers.h:114-127           apply_selection (flavor copy target)
include/agent/config.h:20-84                Config fields (add flavor/account-token flag)
include/agent/config.h:180-181              api_url()/models_url() (delete → dialect)
include/agent/request_builder.h:12-28       build_chat_body/sanitize decls (delete → dialect)
include/agent/sse_parser.h:35-63            StreamParser (→ StreamDecoder base + openai impl)
include/agent/model_probe.h:13-56           probe services + resolve_kilo_balance_token decl
lib/llm.cpp:14-88                           HttpLLMClient (dialect ctor param; body/parse via dialect)
lib/request_builder.cpp:75-183              build_chat_body (moves to dialect_openai.cpp)
lib/http_transport.cpp:56-87,95-98          context sniff + apply_auth (→ dialect), G1/G2 pins
lib/http_transport.cpp:115-152,160-186      message_from_completion + retry policy (→ dialect)
lib/http_transport.cpp:207-249,271-330      curl_exec/post/stream (stays, dialect-driven)
lib/sse_parser.cpp:146-211,215-235          openai event state machine (→ OpenAIStreamDecoder)
lib/model_probe.cpp:59-81,85-180            fetch/parse models (→ dialect-driven)
lib/model_probe.cpp:236-243                 resolve_kilo_balance_token name-branch (delete, 029)
lib/providers_service.cpp:12-21,101-106     capability override table + capabilities() (wire up)
lib/providers_service.cpp:114-127           apply_selection (flavor/flag copy)
lib/providers_repo_static.cpp:13-26         presets (add anthropic row in 030)
lib/providers_catalog_http.cpp:14-24        HttpModelCatalog (dialect-driven probe)
tui/tui_input.cpp:1351-1438,1278-1312       /provider + /set model (unchanged; balance rename site)
Makefile.in:144,293                         UNITTEST_OBJ + run_tests link (add dialect tests)
docs/spec/llm-client/{http-transport,model-probe,streaming}.md   specs to update (031)
docs/spec/plugins/plugin-framework.md    Phase 5 ProviderImpl → registry note (031)
```

Spec credit: `AGENTS.md` Engineering principles + fix workflow, `docs/fix-proposal/clean-architecture-2026-08-27.md` style, prior YAGNI decisions on `Capability void*` and Provider/Memory wire-up deferral.
