
#ifndef AGENT_LLM_H
#define AGENT_LLM_H

#include <string>
#include <vector>
#include <functional>
#include "agent/config.h"
#include "agent/tool.h"

namespace agent {

// A single message in the conversation. role is one of:
//   "system", "user", "assistant", "tool"
struct Message {
    std::string role;
    std::string content;
    std::string reasoning;               // assistant thinking (not sent back)
    std::string tool_call_id;            // for role == "tool"
    std::string name;                    // tool name for tool messages
    json tool_calls;                     // assistant messages may carry calls
};

// One parsed Server-Sent-Event delta from a streaming response.
struct StreamChunk {
    bool done = false;                   // terminal [DONE] marker seen
    std::string delta;                   // incremental answer text (if any)
    std::string reasoning;               // incremental thinking/reasoning text
    json tool_calls;                     // incremental tool_call fragments (if any)
};

// Capabilities discovered from the server's /v1/models endpoint. Fields are
// empty / <=0 when the server did not report them.
struct ServerInfo {
    bool ok = false;            // a model entry was successfully parsed
    std::string model;          // model id (e.g. the gguf name / alias)
    int context_size = 0;       // n_ctx (loaded context window), 0 if unknown
    int context_train = 0;      // n_ctx_train (model's native max), 0 if unknown
};

// Per-request telemetry, surfaced to UIs for the status bar. Filled in after a
// chat/chat_stream call completes. Fields are -1/0 when unknown.
struct Stats {
    double latency_ms = -1;              // time-to-first-byte (server "lag")
    double tps = -1;                     // completion tokens / generation time
    long prompt_tokens = -1;            // usage.prompt_tokens (context used)
    long completion_tokens = -1;        // usage.completion_tokens
    bool valid = false;                  // true once a request populated it
};

// Port: a chat backend over an OpenAI-compatible endpoint. The library does
// not own conversation state; callers pass the full message list each turn.
// Implementations: HttpLLMClient (libcurl) and FakeLLMClient (tests).
// A typed transport failure for the retry policy: `retryable` errors (curl
// failures, timeouts, empty bodies, HTTP 429/5xx) may be retried with
// backoff; non-retryable ones (auth/misconfig 4xx) must fail fast.
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
    virtual Message chat_stream(
        const std::vector<Message>& messages, const std::vector<std::shared_ptr<Tool>>& tools,
        const std::function<void(const StreamChunk&)>& on_chunk,
        Stats* stats = nullptr) = 0;

    // Parse a /v1/models JSON body into ServerInfo. Exposed (and static) so the
    // extraction logic can be unit-tested without a live server. When
    // `preferred_model` is non-empty the matching entry wins (a router may
    // list models without context metadata ahead of the active one); otherwise
    // the first entry that reports a positive context is used, falling back
    // to the first entry.
    static ServerInfo parse_models(const std::string& body,
                                   const std::string& preferred_model = "");

    // Context window the server taught us via a 400 overflow rejection
    // (parse_context_size_from_error), or 0 when none was learned yet.
    // The learned limit is the runtime truth: it clamps any configured or
    // probed window so the gate and gauge never overshoot.
    virtual int learned_context_size() const { return 0; }
};

// Real libcurl implementation over an OpenAI-compatible /chat/completions
// endpoint. Supports both buffered (chat) and streamed (chat_stream) modes.
class HttpLLMClient : public LLMClient {
public:
    explicit HttpLLMClient(Config cfg);

    ServerInfo probe_server() const override;
    Message chat(const std::vector<Message>& messages,
                 const std::vector<std::shared_ptr<Tool>>& tools,
                 Stats* stats = nullptr) override;
    Message chat_stream(
        const std::vector<Message>& messages, const std::vector<std::shared_ptr<Tool>>& tools,
        const std::function<void(const StreamChunk&)>& on_chunk,
        Stats* stats = nullptr) override;
    int learned_context_size() const override { return learned_; }

private:
    Config cfg_;
    int learned_ = 0;  // window taught by a 400 overflow rejection
};

// Merge probed server info into a Config, filling ONLY values the user did not
// set explicitly (model_explicit / context_explicit). Pure and network-free so
// the auto-detect policy can be unit-tested. A non-ok / empty info is a no-op.
void merge_server_info(Config& cfg, const ServerInfo& info);

// Probe the configured server and fill in any auto-detectable Config values
// (model, context_size) that were NOT set explicitly by the user. Returns the
// ServerInfo probed (ok == false if the server was unreachable). Safe to call
// once at startup; never throws. Delegates the merge policy to
// merge_server_info().
ServerInfo apply_server_autodetect(Config& cfg);

} // namespace agent

#endif // AGENT_LLM_H
