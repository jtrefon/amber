
#ifndef AGENT_DIALECT_H
#define AGENT_DIALECT_H

// Port: everything that differs between LLM provider wire protocols.
//
// One implementation per flavor ("openai", ...). The client resolves a
// dialect once at construction (make_dialect(cfg.flavor)); callers never
// branch on flavor or provider names. Adding a provider wire protocol is a
// new Dialect implementation registered in the built-in table — no edits to
// the transport, the agent loop, or the UI.
//
// Dialects normalize to the internal DTOs (Message, StreamChunk, ServerInfo,
// ModelInfo, Stats) at the edge, so history, dispatch, and rendering stay
// provider-agnostic.

#include "agent/config.h"
#include "agent/llm.h"
#include "agent/model_probe.h"
#include "agent/stream_decoder.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace agent {

// Token counts reported by a wire response, -1 when the protocol did not
// report them.
struct TokenUsage {
    long prompt = -1;
    long completion = -1;
};

class Dialect {
public:
    virtual ~Dialect() = default;

    // Flavor identifier this dialect implements ("openai", ...).
    virtual std::string flavor() const = 0;

    // Endpoints derived from the configured api_base.
    virtual std::string chat_url(const Config& cfg) const = 0;
    // Model-listing endpoint. Empty when the protocol has none.
    virtual std::string models_url(const Config& cfg) const = 0;

    // Auth headers for a request (e.g. "Authorization: Bearer <key>").
    virtual std::vector<std::string> auth_headers(const Config& cfg) const = 0;

    // Build the request body. Pure: unit-testable without libcurl.
    virtual json build_chat_body(
        const Config& cfg, const std::vector<Message>& messages,
        const std::vector<std::shared_ptr<Tool>>& tools, bool stream) const = 0;

    // Parse a buffered response body into an assistant message. Degrades
    // gracefully on malformed input (recovery text, never throws).
    virtual Message parse_completion(const std::string& raw) const = 0;

    // Create the incremental decoder for a streamed response.
    virtual std::unique_ptr<StreamDecoder> make_decoder(
        Message& out, StreamDecoder::ChunkSink on_chunk,
        std::string debug_path) const = 0;

    // Parse a model-listing body. `preferred_model` (when non-empty) selects
    // the entry whose id matches; the protocol's own fallback applies
    // otherwise. Pure: unit-testable without libcurl.
    virtual ServerInfo parse_models_response(
        const std::string& body,
        const std::string& preferred_model = "") const = 0;
    virtual std::vector<ModelInfo> parse_model_list_response(
        const std::string& body) const = 0;

    // Token usage of a buffered response body.
    virtual TokenUsage parse_usage(const std::string& raw) const = 0;

    // Error classification: is a non-2xx failure transient (retry) or a
    // genuine rejection? And, when the body reports it, the true context
    // window the server enforced (0 when unknown).
    virtual bool is_retryable(long http_code, const std::string& body) const = 0;
    virtual int context_overflow_hint(const std::string& error_body) const = 0;
};

// Resolve a dialect by flavor. Unknown flavors fall back to "openai" (user
// provider files always default to it), so a typo never breaks a session.
std::unique_ptr<Dialect> make_dialect(const std::string& flavor);

// Register a dialect factory (built-ins register here; future plugin
// providers register into the same table).
void register_dialect(const std::string& flavor,
                      std::function<std::unique_ptr<Dialect>()> factory);

} // namespace agent

#endif // AGENT_DIALECT_H
