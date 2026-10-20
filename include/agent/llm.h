
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
    std::string failure_detail;
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
    std::vector<std::string> supported_parameters;
};

// The completion ask the model will actually accept: the configured budget
// capped by the model's advertised output limit (0 = not advertised).
inline size_t request_max_tokens(const Config& cfg) {
    if (cfg.model_max_output <= 0)
        return cfg.max_tokens;
    return std::min(cfg.max_tokens, static_cast<size_t>(cfg.model_max_output));
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
    std::vector<std::string> input_modalities;
    std::vector<std::string> output_modalities;
    std::vector<std::string> supported_parameters;
};

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
    ApiError(long s, bool r, const std::string& msg)
        : std::runtime_error(msg), status(s), retryable(r) {}
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
