
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
    std::string reasoning;    // assistant thinking (not sent back)
    std::string tool_call_id; // for role == "tool"
    std::string name;         // tool name for tool messages
    json tool_calls;          // assistant messages may carry calls
    // True for a notice the harness itself authored ([harness] prefix): a
    // retry summary, a repair report, a budget note. It rides the user channel
    // because the OpenAI dialect hoists every system message to the head of the
    // request, which would lose a notice's place in the conversation. The flag
    // is what keeps the text from being mistaken for the model's own close.
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
    bool reasoning = false;
    std::string line; // provider family line (Anthropic "line"), informational
    // How the model accepts reasoning. A listing that names none of these is
    // unknown and gets no thinking block: unknown is never guessed.
    bool thinking_adaptive = false;         // thinking.type=adaptive + output_config.effort
    bool thinking_legacy = false;           // thinking.type=enabled + budget_tokens
    bool thinking_always_on = false;        // cannot disable; omitting keeps the default
    bool thinking_disabled_ok = false;      // thinking.type=disabled accepted
    bool thinking_between_tools_ok = false; // thinking.type=between_tools accepted
    std::vector<std::string> efforts;       // accepted output_config.effort values
    std::string default_effort;             // provider default effort
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
    bool reasoning = false;
    std::string line;
    bool thinking_adaptive = false;
    bool thinking_legacy = false;
    bool thinking_always_on = false;
    bool thinking_disabled_ok = false;
    bool thinking_between_tools_ok = false;
    std::vector<std::string> efforts;
    std::string default_effort;
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
    bool reasoning = false;
    std::string line;
    bool thinking_adaptive = false;
    bool thinking_legacy = false;
    bool thinking_always_on = false;
    bool thinking_disabled_ok = false;
    bool thinking_between_tools_ok = false;
    std::vector<std::string> efforts;
    std::string default_effort;
    std::vector<std::string> supported_parameters;
};

inline ModelCapabilities capabilities_of(const ModelInfo& m) {
    return {m.context,
            m.max_output,
            m.reasoning,
            m.line,
            m.thinking_adaptive,
            m.thinking_legacy,
            m.thinking_always_on,
            m.thinking_disabled_ok,
            m.thinking_between_tools_ok,
            m.efforts,
            m.default_effort,
            m.supported_parameters};
}

inline ModelCapabilities capabilities_of(const ServerInfo& s) {
    return {s.context_size,
            s.max_output,
            s.reasoning,
            s.line,
            s.thinking_adaptive,
            s.thinking_legacy,
            s.thinking_always_on,
            s.thinking_disabled_ok,
            s.thinking_between_tools_ok,
            s.efforts,
            s.default_effort,
            s.supported_parameters};
}

inline ModelCapabilities capabilities_of(const Config& c) {
    return {c.context_size,
            c.model_max_output,
            c.model_supports_reasoning,
            c.model_line,
            c.model_thinking_adaptive,
            c.model_thinking_legacy,
            c.model_thinking_always_on,
            c.model_thinking_disabled_ok,
            c.model_thinking_between_tools_ok,
            c.model_efforts,
            c.model_default_effort,
            c.model_supported_parameters};
}

inline bool has_capabilities(const ModelCapabilities& c) {
    return c.context > 0 || c.max_output > 0 || c.reasoning || !c.line.empty() ||
           c.thinking_adaptive || c.thinking_legacy || c.thinking_always_on ||
           c.thinking_disabled_ok || c.thinking_between_tools_ok || !c.efforts.empty() ||
           !c.default_effort.empty() || !c.supported_parameters.empty();
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
};

inline constexpr const char* kClaudeEffortsAll[] = {"low",   "medium", "high",
                                                    "xhigh", "max",    nullptr};
inline constexpr const char* kClaudeEffortsNoXhigh[] = {"low", "medium", "high", "max", nullptr};
inline constexpr const char* kClaudeEffortsLowMediumHigh[] = {"low", "medium", "high", nullptr};

inline constexpr ClaudeFamily kClaudeFamilies[] = {
    // 5.x: 1M input, 128K output, adaptive thinking. The Opus/Fable/Haiku
    // flagships are always-on; the Sonnet pair can still be disabled.
    {"claude-fable-5-1", 1000000, 128000, true, true, false, true, kClaudeEffortsAll, "high"},
    {"claude-fable-5", 1000000, 128000, true, true, false, true, kClaudeEffortsAll, "high"},
    {"claude-opus-5-5", 1000000, 128000, true, true, false, true, kClaudeEffortsAll, "medium"},
    {"claude-opus-5", 1000000, 128000, true, true, false, true, kClaudeEffortsAll, "high"},
    {"claude-sonnet-5-5", 1000000, 128000, true, true, false, false, kClaudeEffortsAll, "high"},
    {"claude-sonnet-5", 1000000, 128000, true, true, false, false, kClaudeEffortsAll, "high"},
    {"claude-haiku-5-5", 1000000, 128000, true, true, false, true, kClaudeEffortsAll, "medium"},
    // 4.8/4.7: 1M, adaptive, xhigh available.
    {"claude-opus-4-8", 1000000, 128000, true, true, false, false, kClaudeEffortsAll, "high"},
    {"claude-opus-4-7", 1000000, 128000, true, true, false, false, kClaudeEffortsAll, "high"},
    // 4.6: 1M, adaptive, max available but not xhigh.
    {"claude-opus-4-6", 1000000, 128000, true, true, false, false, kClaudeEffortsNoXhigh, "high"},
    {"claude-sonnet-4-6", 1000000, 128000, true, true, false, false, kClaudeEffortsNoXhigh, "high"},
    // 4.5: 200K, legacy thinking budget. Opus 4.5 additionally has effort
    // control; Sonnet/Haiku 4.5 do not.
    {"claude-opus-4-5", 200000, 64000, true, false, true, false, kClaudeEffortsLowMediumHigh,
     "high"},
    {"claude-sonnet-4-5", 200000, 64000, true, false, true, false, nullptr, ""},
    {"claude-haiku-4-5", 200000, 64000, true, false, true, false, nullptr, ""},
    // 4.1/4.0: 200K, legacy thinking budget.
    {"claude-opus-4-1", 200000, 32000, true, false, true, false, nullptr, ""},
    {"claude-sonnet-4", 200000, 64000, true, false, true, false, nullptr, ""},
    {"claude-opus-4", 200000, 32000, true, false, true, false, nullptr, ""},
    {"claude-haiku-4", 200000, 64000, true, false, true, false, nullptr, ""},
    // 3.x.
    {"claude-3-7", 200000, 64000, true, false, true, false, nullptr, ""},
    {"claude-3-5", 200000, 8192, false, false, false, false, nullptr, ""},
    {"claude-3-opus", 200000, 4096, false, false, false, false, nullptr, ""},
    {"claude-3-sonnet", 200000, 4096, false, false, false, false, nullptr, ""},
    {"claude-3-haiku", 200000, 4096, false, false, false, false, nullptr, ""},
    {"claude-2.1", 200000, 4096, false, false, false, false, nullptr, ""},
    {"claude-2", 100000, 4096, false, false, false, false, nullptr, ""},
    {"claude-instant", 100000, 4096, false, false, false, false, nullptr, ""},
};

// The vendor prefix some gateways put in front of the model id
// (`anthropic/claude-sonnet-5`). Family matching looks at the basename so the
// same table serves the Anthropic listing and an OpenAI-compatible gateway.
inline const ClaudeFamily* claude_family_for(const std::string& id) {
    const std::size_t slash = id.rfind('/');
    const std::string name = slash == std::string::npos ? id : id.substr(slash + 1);
    const ClaudeFamily* best = nullptr;
    std::size_t best_len = 0;
    for (const auto& family : kClaudeFamilies) {
        const std::string prefix(family.prefix);
        if (name.rfind(prefix, 0) != 0 || prefix.size() <= best_len)
            continue;
        best = &family;
        best_len = prefix.size();
    }
    return best;
}

inline void apply_claude_family(ModelInfo& m) {
    const ClaudeFamily* f = claude_family_for(m.id);
    if (!f)
        return;
    if (m.context == 0)
        m.context = f->context;
    if (m.max_output == 0)
        m.max_output = f->max_output;
    m.reasoning = m.reasoning || f->reasoning;
    if (!m.thinking_adaptive && !m.thinking_legacy && !m.thinking_always_on) {
        m.thinking_adaptive = f->adaptive;
        m.thinking_legacy = f->legacy;
        m.thinking_always_on = f->always_on;
    }
    if (m.efforts.empty() && f->efforts)
        for (const char* const* p = f->efforts; *p; ++p)
            m.efforts.emplace_back(*p);
    if (m.default_effort.empty() && f->default_effort)
        m.default_effort = f->default_effort;
    if (m.thinking_adaptive)
        m.thinking_between_tools_ok = true;
    if (m.thinking_adaptive && !m.thinking_always_on)
        m.thinking_disabled_ok = true;
}

inline void apply_claude_family(ServerInfo& s) {
    const ClaudeFamily* f = claude_family_for(s.model);
    if (!f)
        return;
    if (s.context_size == 0)
        s.context_size = f->context;
    if (s.max_output == 0)
        s.max_output = f->max_output;
    s.reasoning = s.reasoning || f->reasoning;
    if (!s.thinking_adaptive && !s.thinking_legacy && !s.thinking_always_on) {
        s.thinking_adaptive = f->adaptive;
        s.thinking_legacy = f->legacy;
        s.thinking_always_on = f->always_on;
    }
    if (s.efforts.empty() && f->efforts)
        for (const char* const* p = f->efforts; *p; ++p)
            s.efforts.emplace_back(*p);
    if (s.default_effort.empty() && f->default_effort)
        s.default_effort = f->default_effort;
    if (s.thinking_adaptive)
        s.thinking_between_tools_ok = true;
    if (s.thinking_adaptive && !s.thinking_always_on)
        s.thinking_disabled_ok = true;
}

inline void apply_model_capabilities(Config& cfg, const ModelCapabilities& c) {
    cfg.model_max_output = c.max_output;
    cfg.model_supports_reasoning = c.reasoning;
    cfg.model_supported_parameters = c.supported_parameters;
    cfg.model_line = c.line;
    cfg.model_thinking_adaptive = c.thinking_adaptive;
    cfg.model_thinking_legacy = c.thinking_legacy;
    cfg.model_thinking_always_on = c.thinking_always_on;
    cfg.model_thinking_disabled_ok = c.thinking_disabled_ok;
    cfg.model_thinking_between_tools_ok = c.thinking_between_tools_ok;
    cfg.model_efforts = c.efforts;
    cfg.model_default_effort = c.default_effort;
}

// Per-request telemetry, surfaced to UIs for the status bar. Filled in after a
// chat/chat_stream call completes. Fields are -1/0 when unknown.
struct Stats {
    double latency_ms = -1;      // time-to-first-byte (server "lag")
    double tps = -1;             // completion tokens / generation time
    long prompt_tokens = -1;     // usage.prompt_tokens (context used)
    long completion_tokens = -1; // usage.completion_tokens
    bool valid = false;          // true once a request populated it
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

private:
    void add_notice(std::string text);

    Config cfg_;
    std::unique_ptr<Dialect> dialect_;
    int learned_ = 0; // window taught by a 400 overflow rejection
    std::mutex notices_mtx_;
    std::vector<std::string> notices_;
};

// Merge probed server info into a Config, filling ONLY values the user did not
// set explicitly (model_explicit / context_explicit). Pure and network-free so
// the auto-detect policy can be unit-tested. A non-ok / empty info is a no-op.
void merge_server_info(Config& cfg, const ServerInfo& info);

// Fill in any auto-detectable Config values (model, context_size) that were
// NOT set explicitly by the user, from the model catalog. Returns the
// ServerInfo parsed (ok == false when no catalog entry parses). Blocking
// (cache-through, single-flight); force=true always revalidates over the
// network — see model_probe.h for the cache and the non-blocking variants.
ServerInfo apply_server_autodetect(Config& cfg, bool force = false);

} // namespace agent

#endif // AGENT_LLM_H
