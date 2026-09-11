
#ifndef AGENT_MODEL_PROBE_H
#define AGENT_MODEL_PROBE_H

#include "agent/config.h"
#include "agent/llm.h"

namespace agent {

class Dialect;

// Query the server's model-listing endpoint and report the active model's id
// and context window (n_ctx). Never throws: on any transport/parse failure it
// returns a ServerInfo with ok == false. The endpoint, auth, and parsing are
// the dialect's (resolved from cfg.flavor); the two-argument overload uses a
// caller-held dialect (the client's).
ServerInfo probe_server(const Config& cfg);
ServerInfo probe_server(const Config& cfg, const Dialect& dialect);

// Fetch all model entries with context info from the configured server.
// Returns empty on error (including a protocol without a listing endpoint).
std::vector<ModelInfo> list_model_info(const Config& cfg);
std::vector<ModelInfo> list_model_info(const Config& cfg, const Dialect& dialect);

// Fetch all model IDs from the configured server. Returns empty on error.
std::vector<std::string> list_models(const Config& cfg);

// The kilo.ai balance readout lives with its provider: see
// plugins/kilocode/kilocode_plugin.h. Core probing knows nothing about it.

} // namespace agent

#endif // AGENT_MODEL_PROBE_H
