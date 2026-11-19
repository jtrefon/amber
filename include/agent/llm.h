
#ifndef AGENT_LLM_H
#define AGENT_LLM_H

#include <algorithm>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <mutex>
#include "agent/config.h"
#include "agent/tool.h"

namespace agent {

class Dialect;

// A single message in the conversation. role is one of:
//   "system", "user", "assistant", "tool"
struct Message {
    std::string role;
    std::string content;
    // Assistant thinking. Replayed on later turns when the provider requires it
    // (DeepSeek's reasoning_content, OpenRouter's reasoning string); otherwise
    // it stays local to the UI.
    std::string reasoning;
    std::string tool_call_id; // for role == "tool"
    std::string name;         // tool name for tool messages
    json tool_calls;          // assistant messages may carry calls
    // True for a notice the harness itself authored ([harness] prefix): a
    // retry summary, a repair report, a budget note. It rides the user channel
    // because every dialect hoists system messages into the leading block: a
    // notice must keep its place in the conversation, and the same rule keeps a
    // volatile prompt block (the session brief, an activated skill body, a
    // plugin tail block) out of the cached prefix. The flag is what keeps the
    // text from being mistaken for the model's own close.
    bool harness = false;
    // When the harness authored this notice because a request failed, the raw
    // failure it reports (transport error, HTTP status). Empty for ordinary
    // guidance, and deliberately not serialized: it describes the turn, not the
    // conversation.
    std::string failure_detail{};
    // A rate-limit failure is a stop signal, not a retryable turn: the status
    // and Retry-After let the loop end instead of asking the model to continue.
    long failure_status = 0;
    long failure_retry_after_ms = 0;
    // The assistant turn exactly as a protocol sent it, when the protocol has
    // content blocks that cannot be reconstructed from text and tool calls.
    // Anthropic signs thinking blocks and rejects a tool-use turn that replays
    // them modified or omits them, so the raw blocks travel with the message.
    // Protocols without such blocks leave this null and never read it.
    json raw_content{};
    // The provider's structured reasoning blocks, replayed verbatim when the
    // listing asks for them (OpenRouter's reasoning_details). Empty when the
    // protocol has no such shape.
    json reasoning_details{};
    // The assistant-message field the reasoning actually arrived in
    // (reasoning_content, reasoning, ...). A cold catalogue can still replay
    // the provider's own field instead of guessing one.
    std::string reasoning_field;
};

// One parsed Server-Sent-Event delta from a streaming response.
struct StreamChunk {
    bool done = false;     // terminal [DONE] marker seen
    std::string delta;     // incremental answer text (if any)
    std::string reasoning; // incremental thinking/reasoning text
    json tool_calls;       // incremental tool_call fragments (if any)
};

// Capabilities discovered from the server's /v1/models endpoint. Fields are
// empty / <=0 when the server did not report them.
struct ServerInfo {
    bool ok = false;       // a model entry was successfully parsed
    std::string model;     // model id (e.g. the gguf name / alias)
    int context_size = 0;  // n_ctx (loaded context window), 0 if unknown
    int context_train = 0; // n_ctx_train (model's native max), 0 if unknown
    int max_output = 0;    // max completion tokens, 0 if unknown
    // How the model accepts reasoning. A listing that names nothing is unknown
    // and gets no field: unknown is never guessed.
    ReasoningContract reasoning;
    std::string line; // provider family line (Anthropic "line"), informational
    std::vector<std::string> supported_parameters;
};

// The completion ask the model will actually accept: the configured budget
// capped by the model's advertised output limit (0 = not advertised).
inline size_t request_max_tokens(const Config& cfg) {
    if (cfg.model_max_output <= 0)
        return cfg.max_tokens;
    return std::min(cfg.max_tokens, static_cast<size_t>(cfg.model_max_output));
}

// A non-empty advertised parameter list that omits tools means the model is not
// a tool caller; an empty list is unknown, so tools are sent. Respecting the
// listing avoids offering a tool schema the endpoint never advertised.
inline bool tools_advertised(const Config& cfg) {
    if (cfg.model_supported_parameters.empty())
        return true;
    return std::find(cfg.model_supported_parameters.begin(), cfg.model_supported_parameters.end(),
                     "tools") != cfg.model_supported_parameters.end();
}

// One entry of a provider's model listing, as parsed by a Dialect. `context`
// is 0 when the protocol does not report a window and no known-family fallback
// applies. The capability fields are 0/empty/false when the listing is silent:
// unknown is never guessed.
struct ModelInfo {
    std::string id;
    int context = 0;       // input context window
    int context_train = 0; // native max
    int max_output = 0;    // max completion/output tokens
    ReasoningContract reasoning;
    std::string line;
    std::vector<std::string> input_modalities;
    std::vector<std::string> output_modalities;
    std::vector<std::string> supported_parameters;
};

// The capability set a model hand-off carries. A struct rather than another
// five parameters: the request shape and the picker read the same fields, and
// a positional signature this wide is how two of them silently swapped.
struct ModelCapabilities {
    int context = 0;
    int max_output = 0;
    ReasoningContract reasoning;
    std::string line;
    std::vector<std::string> supported_parameters;
};

inline ModelCapabilities capabilities_of(const ModelInfo& m) {
    return {m.context, m.max_output, m.reasoning, m.line, m.supported_parameters};
}

inline ModelCapabilities capabilities_of(const ServerInfo& s) {
    return {s.context_size, s.max_output, s.reasoning, s.line, s.supported_parameters};
}

inline ModelCapabilities capabilities_of(const Config& c) {
    return {c.context_size, c.model_max_output, c.model_reasoning, c.model_line,
            c.model_supported_parameters};
}

inline bool has_capabilities(const ModelCapabilities& c) {
    return c.context > 0 || c.max_output > 0 || c.reasoning.supported || !c.line.empty() ||
           !c.supported_parameters.empty();
}

// Documented Claude family defaults. A listing is authoritative; these fill
// only the fields it omitted, which is how a proxy that returns bare ids still
// yields a usable window and request shape. Prefixes are matched longest-first
// so `claude-opus-4-5` never resolves as `claude-opus-4`.
struct ClaudeFamily {
    const char* prefix;
    int context;
    int max_output;
    bool reasoning;
    bool adaptive;              // thinking.type=adaptive + output_config.effort
    bool legacy;                // thinking.type=enabled + budget_tokens
    bool always_on;             // cannot be disabled
    const char* const* efforts; // nullptr-terminated; nullptr = no effort control
    const char* default_effort;
    // The provider's documented minimum cacheable prefix, in tokens. A shorter
    // prompt is accepted and cached *not at all* -- no error, both counters 0 --
    // which is indistinguishable from a client that sent no breakpoint. Kept
    // beside the family because it is a per-model vendor constant, and a stale
    // value only ever produces a wrong diagnostic sentence, never a wrong
    // request.
    int min_cacheable;
};

inline constexpr const char* kClaudeEffortsAll[] = {"low",   "medium", "high",
                                                    "xhigh", "max",    nullptr};
inline constexpr const char* kClaudeEffortsNoXhigh[] = {"low", "medium", "high", "max", nullptr};
inline constexpr const char* kClaudeEffortsLowMediumHigh[] = {"low", "medium", "high", nullptr};

inline constexpr ClaudeFamily kClaudeFamilies[] = {
    // 5.x: 1M input, 128K output, adaptive thinking. The Opus/Fable/Haiku
    // flagships are always-on; the Sonnet pair can still be disabled.
    {"claude-fable-5-1", 1000000, 128000, true, true, false, true, kClaudeEffortsAll, "high", 512},
    {"claude-fable-5", 1000000, 128000, true, true, false, true, kClaudeEffortsAll, "high", 512},
    {"claude-opus-5-5", 1000000, 128000, true, true, false, true, kClaudeEffortsAll, "medium", 512},
    {"claude-opus-5", 1000000, 128000, true, true, false, true, kClaudeEffortsAll, "high", 512},
    {"claude-sonnet-5-5", 1000000, 128000, true, true, false, false, kClaudeEffortsAll, "high",
     512},
    {"claude-sonnet-5", 1000000, 128000, true, true, false, false, kClaudeEffortsAll, "high", 1024},
    {"claude-haiku-5-5", 1000000, 128000, true, true, false, true, kClaudeEffortsAll, "medium",
     512},
    // 4.8/4.7: 1M, adaptive, xhigh available.
    {"claude-opus-4-8", 1000000, 128000, true, true, false, false, kClaudeEffortsAll, "high", 1024},
    {"claude-opus-4-7", 1000000, 128000, true, true, false, false, kClaudeEffortsAll, "high", 2048},
    // 4.6: 1M, adaptive, max available but not xhigh.
    {"claude-opus-4-6", 1000000, 128000, true, true, false, false, kClaudeEffortsNoXhigh, "high",
     4096},
    {"claude-sonnet-4-6", 1000000, 128000, true, true, false, false, kClaudeEffortsNoXhigh, "high",
     1024},
    // 4.5: 200K, legacy thinking budget. Opus 4.5 additionally has effort
    // control; Sonnet/Haiku 4.5 do not.
    {"claude-opus-4-5", 200000, 64000, true, false, true, false, kClaudeEffortsLowMediumHigh,
     "high", 4096},
    {"claude-sonnet-4-5", 200000, 64000, true, false, true, false, nullptr, "", 1024},
    {"claude-haiku-4-5", 200000, 64000, true, false, true, false, nullptr, "", 4096},
    // 4.1/4.0: 200K, legacy thinking budget.
    {"claude-opus-4-1", 200000, 32000, true, false, true, false, nullptr, "", 1024},
    {"claude-sonnet-4", 200000, 64000, true, false, true, false, nullptr, "", 1024},
    {"claude-opus-4", 200000, 32000, true, false, true, false, nullptr, "", 1024},
    {"claude-haiku-4", 200000, 64000, true, false, true, false, nullptr, "", 1024},
    // 3.x.
    {"claude-3-7", 200000, 64000, true, false, true, false, nullptr, "", 1024},
    {"claude-3-5", 200000, 8192, false, false, false, false, nullptr, "", 1024},
    {"claude-3-opus", 200000, 4096, false, false, false, false, nullptr, "", 1024},
    {"claude-3-sonnet", 200000, 4096, false, false, false, false, nullptr, "", 1024},
    {"claude-3-haiku", 200000, 4096, false, false, false, false, nullptr, "", 2048},
    {"claude-2.1", 200000, 4096, false, false, false, false, nullptr, "", 1024},
    {"claude-2", 100000, 4096, false, false, false, false, nullptr, "", 1024},
    {"claude-instant", 100000, 4096, false, false, false, false, nullptr, "", 1024},
};

// The vendor prefix some gateways put in front of the model id
// (`anthropic/claude-sonnet-5`) and the `:free`/`:batch` variants OpenRouter
// appends. Family matching looks at the basename, strips the variant suffix,
// and treats a dotted version (`claude-opus-5.5`) like the hyphenated native
// id (`claude-opus-5-5`) so one table serves both listings.
inline std::string model_basename(const std::string& id) {
    const std::size_t slash = id.rfind('/');
    std::string name = slash == std::string::npos ? id : id.substr(slash + 1);
    const std::size_t colon = name.find(':');
    if (colon != std::string::npos)
        name.erase(colon);
    return name;
}

inline std::string family_name(const std::string& id) {
    std::string name = model_basename(id);
    std::replace(name.begin(), name.end(), '.', '-');
    return name;
}

inline const ClaudeFamily* claude_family_for(const std::string& id) {
    const std::string name = family_name(id);
    const ClaudeFamily* best = nullptr;
    std::size_t best_len = 0;
    for (const auto& family : kClaudeFamilies) {
        std::string prefix(family.prefix);
        std::replace(prefix.begin(), prefix.end(), '.', '-');
        if (name.rfind(prefix, 0) != 0 || prefix.size() <= best_len)
            continue;
        best = &family;
        best_len = prefix.size();
    }
    return best;
}

// The provider's minimum cacheable prefix for `id`, or 0 when the model is not
// a documented Claude family. A request below it is cached not at all: the API
// accepts the breakpoint, returns no error, and reports 0/0 -- the state a
// provider dashboard shows as "unknown", and the one thing a client cannot
// otherwise tell apart from having sent no breakpoint.
inline int min_cacheable_tokens(const std::string& id) {
    const ClaudeFamily* family = claude_family_for(id);
    return family ? family->min_cacheable : 0;
}

inline void apply_claude_family_reasoning(ReasoningContract& r, const ClaudeFamily& f) {
    r.supported = r.supported || f.reasoning;
    if (f.reasoning && (f.adaptive || f.always_on))
        r.default_enabled = true;
    if (f.adaptive || f.legacy || f.efforts)
        r.effort_levels = true;
    if (!r.adaptive && !r.legacy && !r.mandatory) {
        r.adaptive = f.adaptive;
        r.legacy = f.legacy;
        r.mandatory = f.always_on;
    }
    if (r.efforts.empty() && f.efforts)
        for (const char* const* p = f.efforts; *p; ++p)
            r.efforts.emplace_back(*p);
    if (r.default_effort.empty() && f.default_effort)
        r.default_effort = f.default_effort;
    if (r.adaptive)
        r.between_tools_ok = true;
    if (r.adaptive && !r.mandatory)
        r.disabled_ok = true;
    if (r.mandatory)
        r.off = ReasoningOff::Unsupported;
    else if (r.disabled_ok || r.between_tools_ok)
        r.off = ReasoningOff::ReasoningDisabled;
}

inline void apply_claude_family(ModelInfo& m) {
    const ClaudeFamily* f = claude_family_for(m.id);
    if (!f)
        return;
    if (m.context == 0)
        m.context = f->context;
    if (m.max_output == 0)
        m.max_output = f->max_output;
    apply_claude_family_reasoning(m.reasoning, *f);
}

inline void apply_claude_family(ServerInfo& s) {
    const ClaudeFamily* f = claude_family_for(s.model);
    if (!f)
        return;
    if (s.context_size == 0)
        s.context_size = f->context;
    if (s.max_output == 0)
        s.max_output = f->max_output;
    apply_claude_family_reasoning(s.reasoning, *f);
}

// Documented OpenAI families. Their /v1/models listing is deliberately bare,
// so the family table is the only catalogue source for the request shape. It
// fills only a contract the listing did not provide, and the request builder
// never reads a model id. Levels stay empty: the listing named none, so the
// configured level is sent without a clamp.
struct OpenAIFamily {
    const char* prefix;
};

inline constexpr OpenAIFamily kOpenAIFamilies[] = {
    {"o1"},
    {"o3"},
    {"o4"},
    {"gpt-5"},
};

inline void apply_openai_family_reasoning(ReasoningContract& r) {
    if (r.supported)
        return;
    r.supported = true;
    r.effort_levels = true;
    r.wire = ReasoningWire::Effort;
    r.mandatory = true;       // no documented off shape on the bare listing
    r.default_enabled = true; // reasoning is on unless the request says otherwise
    r.off = ReasoningOff::Omit;
}

inline void apply_openai_family(ModelInfo& m) {
    const std::string name = family_name(m.id);
    for (const auto& family : kOpenAIFamilies)
        if (name.rfind(family.prefix, 0) == 0) {
            apply_openai_family_reasoning(m.reasoning);
            return;
        }
}

inline void apply_model_capabilities(Config& cfg, const ModelCapabilities& c) {
    cfg.model_max_output = c.max_output;
    cfg.model_supported_parameters = c.supported_parameters;
    cfg.model_line = c.line;
    cfg.model_reasoning = c.reasoning;
}

// The reasoning levels the protocol understands. A listing narrows this set;
// it does not invent a new spelling.
inline constexpr const char* kReasoningEfforts[] = {"off",  "minimal", "low", "medium",
                                                    "high", "xhigh",   "max", nullptr};

inline bool is_reasoning_effort(const std::string& effort) {
    for (const char* const* p = kReasoningEfforts; *p; ++p)
        if (effort == *p)
            return true;
    return false;
}

// Whether the active dialect has a field it can actually send for this model.
// An OpenAI-compatible request needs a wire shape; the Messages dialect speaks
// the adaptive/legacy shapes even though it has no `reasoning_effort` field.
inline bool reasoning_control_available(const Config& cfg) {
    const ReasoningContract& r = cfg.model_reasoning;
    if (!r.supported)
        return false;
    if (r.wire != ReasoningWire::None)
        return true;
    return cfg.flavor == "anthropic" && (r.adaptive || r.legacy);
}

// The level the next request will actually send. Empty means "send no field":
// the model cannot disable reasoning, the listing named no level, or the
// configured value is not selectable. "on" means the model reasons at its
// default and the request cannot turn it off.
inline std::string effective_reasoning_effort(const ReasoningContract& r,
                                              const std::string& configured) {
    if (!r.supported)
        return {};
    if (configured.empty() || configured == "off") {
        if (r.mandatory || r.off == ReasoningOff::Unsupported)
            return "on";
        if (r.off == ReasoningOff::Omit)
            return r.legacy ? "off" : (r.default_enabled ? "on" : "");
        return "off";
    }
    if (!r.effort_levels)
        return {};
    if (r.efforts.empty())
        return configured;
    if (std::find(r.efforts.begin(), r.efforts.end(), configured) != r.efforts.end())
        return configured;
    // The listing named levels but no default: an unlisted configured value is
    // not sent. Omitting is the conservative answer; inventing one of the
    // listed levels could silently disable reasoning (OpenCode's first level is
    // often "none").
    return r.default_effort;
}

inline std::string effective_reasoning_effort(const Config& cfg) {
    if (!reasoning_control_available(cfg))
        return cfg.model_reasoning.supported ? "on" : "";
    return effective_reasoning_effort(cfg.model_reasoning, cfg.reasoning_effort);
}

// Whether the model can be asked for this level. `off` is selectable when the
// model advertises a way to disable reasoning; a level is selectable when the
// listing named it (or named no levels, in which case the configured value is
// sent as-is).
inline bool reasoning_effort_allowed(const Config& cfg, const std::string& effort) {
    const ReasoningContract& r = cfg.model_reasoning;
    if (!is_reasoning_effort(effort))
        return false;
    if (!reasoning_control_available(cfg))
        return effort == "off";
    if (effort == "off")
        return !r.mandatory && r.off != ReasoningOff::Unsupported;
    if (!r.effort_levels)
        return false;
    if (r.efforts.empty())
        return true;
    return std::find(r.efforts.begin(), r.efforts.end(), effort) != r.efforts.end();
}

// Per-request telemetry, surfaced to UIs for the status bar. Filled in after a
// chat/chat_stream call completes. Fields are -1/0 when unknown.
struct Stats {
    double latency_ms = -1;      // time-to-first-byte (server "lag")
    double tps = -1;             // completion tokens / generation time
    long prompt_tokens = -1;     // usage.prompt_tokens (context used)
    long completion_tokens = -1; // usage.completion_tokens
    // Prompt-cache counters (-1 when the provider did not report them).
    // `cache_read_tokens` is the part of the prompt served from cache; a
    // stable prefix should make it grow with the conversation instead of
    // sitting at 0.
    long cache_read_tokens = -1;
    long cache_write_tokens = -1;
    // The provider's own id for the response, and its explanation when the
    // cache prefix diverged from the previous request.
    std::string response_id;
    std::string cache_miss_reason;
    bool valid = false; // true once a request populated it
};

// Port: a chat backend over an OpenAI-compatible endpoint. The library does
// not own conversation state; callers pass the full message list each turn.
// Implementations: HttpLLMClient (libcurl) and FakeLLMClient (tests).
// A typed transport failure for the retry policy: `retryable` errors (curl
// failures, timeouts, empty bodies, HTTP 429/5xx) may be retried with
// backoff; non-retryable ones (auth/misconfig 4xx) must fail fast.
// Thrown when the caller's CancellationToken fires mid-request. Distinct
// from ApiError so cancellation is never classified as a retryable failure
// and never degrades into a fabricated assistant message.
struct CancelledError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct ApiError : std::runtime_error {
    long status = 0;
    bool retryable = true;
    long retry_after_ms = 0; // Retry-After header, 0 when absent
    ApiError(long s, bool r, const std::string& msg, long retry_after = 0)
        : std::runtime_error(msg), status(s), retryable(r), retry_after_ms(retry_after) {}
};

class LLMClient {
public:
    virtual ~LLMClient() = default;

    // Query the server's GET /v1/models endpoint and report the first model's
    // id and context window (n_ctx). Never throws: on any transport/parse
    // failure it returns a ServerInfo with ok == false.
    virtual ServerInfo probe_server() const = 0;

    // Buffered request: returns the full assistant message (may include
    // tool_calls). Throws std::runtime_error on transport/API failure. When
    // `stats` is non-null it is filled with per-request telemetry.
    virtual Message chat(const std::vector<Message>& messages,
                         const std::vector<std::shared_ptr<Tool>>& tools,
                         Stats* stats = nullptr) = 0;

    // Streaming request: invokes `on_chunk` for every parsed SSE event and
    // returns the assembled assistant message. The callback receives partial
    // text as it arrives, enabling live TUI rendering. When `stats` is non-null
    // it is filled with per-request telemetry after completion.
    virtual Message chat_stream(const std::vector<Message>& messages,
                                const std::vector<std::shared_ptr<Tool>>& tools,
                                const std::function<void(const StreamChunk&)>& on_chunk,
                                Stats* stats = nullptr) = 0;

    // Context window the server taught us via a 400 overflow rejection
    // (parse_context_size_from_error), or 0 when none was learned yet.
    // The learned limit is the runtime truth: it clamps any configured or
    // probed window so the gate and gauge never overshoot.
    virtual int learned_context_size() const { return 0; }

    // Facts the client accumulated while working that the model has a right to
    // know — a window the server clamped, tool calls it had to drop. The agent
    // drains these into [harness] notices, so nothing about a request changes
    // behind the model's back. Draining empties the list.
    virtual std::vector<std::string> take_notices() { return {}; }

    // The conversation id a provider may ask for in a request header. The host
    // calls this when its window/session identity changes; a client that does
    // not speak HTTP ignores it, and the default keeps injected test clients
    // untouched.
    virtual void set_session_id(const std::string& id) { (void)id; }
};

// Real libcurl implementation. The wire protocol (URLs, auth, request body,
// response parsing, streaming, model listing, error classification) is the
// injected Dialect's; this class owns the transport and the assembled
// conversation view. Supports both buffered (chat) and streamed (chat_stream)
// modes.
class HttpLLMClient : public LLMClient {
public:
    explicit HttpLLMClient(Config cfg);
    // `dialect` drives the wire protocol; the one-argument form resolves
    // make_dialect(cfg.flavor). Pass one explicitly in tests or for future
    // plugin providers.
    HttpLLMClient(Config cfg, std::unique_ptr<Dialect> dialect);
    ~HttpLLMClient() override;

    ServerInfo probe_server() const override;
    Message chat(const std::vector<Message>& messages,
                 const std::vector<std::shared_ptr<Tool>>& tools, Stats* stats = nullptr) override;
    Message chat_stream(const std::vector<Message>& messages,
                        const std::vector<std::shared_ptr<Tool>>& tools,
                        const std::function<void(const StreamChunk&)>& on_chunk,
                        Stats* stats = nullptr) override;
    int learned_context_size() const override { return learned_; }
    std::vector<std::string> take_notices() override;
    // Carried into every subsequent request, so a host that assigns the
    // window's identity after construction still sends it.
    void set_session_id(const std::string& id) override { cfg_.session_id = id; }

private:
    void add_notice(std::string text);
    // A 400 that names a cache field means the provider (or a proxy) will not
    // take that field. Turn the narrowest one off, leave a notice, and let the
    // caller retry once. False means the error was not about the cache shape.
    bool degrade_cache_for(const ApiError& error);

    Config cfg_;
    std::unique_ptr<Dialect> dialect_;
    int learned_ = 0; // window taught by a 400 overflow rejection
    std::mutex notices_mtx_;
    std::vector<std::string> notices_;
};

// A one-line warning when a user-pinned context window contradicts the model
// listing by more than 10%. Empty when there is no pin, the listing has no
// window, or the values agree. The pin is never overridden; the warning makes
// an informed override possible.
std::string context_pin_mismatch(const Config& cfg, const ServerInfo& info);

// Merge probed server info into a Config, filling ONLY values the user did not
// set explicitly (model_explicit / context_explicit). Pure and network-free so
// the auto-detect policy can be unit-tested. A non-ok / empty info is a no-op.
// `note` optionally receives `context_pin_mismatch()` for the host to surface.
void merge_server_info(Config& cfg, const ServerInfo& info, std::string* note = nullptr);

// Fill in any auto-detectable Config values (model, context_size) that were
// NOT set explicitly by the user, from the model catalog. Returns the
// ServerInfo parsed (ok == false when no catalog entry parses). Blocking
// (cache-through, single-flight); force=true always revalidates over the
// network — see model_probe.h for the cache and the non-blocking variants.
ServerInfo apply_server_autodetect(Config& cfg, bool force = false);

} // namespace agent

#endif // AGENT_LLM_H
