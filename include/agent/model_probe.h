
#ifndef AGENT_MODEL_PROBE_H
#define AGENT_MODEL_PROBE_H

#include "agent/config.h"
#include "agent/llm.h"

namespace agent {

// Query the server's GET /v1/models endpoint and report the first model's id
// and context window (n_ctx). Never throws: on any transport/parse failure it
// returns a ServerInfo with ok == false.
ServerInfo probe_server(const Config& cfg);

// Parse a /v1/models JSON body into ServerInfo. Pure and network-free so the
// extraction logic can be unit-tested without a live server.
ServerInfo parse_models(const std::string& body);

// Return all model IDs from a /v1/models response. Pure, no network.
std::vector<std::string> parse_model_list(const std::string& body);

// Fetch all model IDs from the configured server. Returns empty on error.
std::vector<std::string> list_models(const Config& cfg);

// One model entry with its context window info (0 when the server did not
// report it). Used by the TUI /set model drawer to show "id (ctx N)" inline.
struct ModelInfo {
    std::string id;
    int context = 0;        // n_ctx (loaded context window)
    int context_train = 0;  // n_ctx_train (native max)
};

// Parse every model entry from a /v1/models response, keeping per-model
// context info. Pure and network-free so it can be unit-tested.
std::vector<ModelInfo> parse_model_list_info(const std::string& body);

// Fetch all model entries with context info from the configured server.
// Returns empty on error.
std::vector<ModelInfo> list_model_info(const Config& cfg);

} // namespace agent

#endif // AGENT_MODEL_PROBE_H
