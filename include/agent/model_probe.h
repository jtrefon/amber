
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

// One model entry with its context window info (0 when the server did not
// report it). Used by the TUI /set model drawer to show "id (ctx N)" inline.
struct ModelInfo {
    std::string id;
    int context = 0;        // n_ctx (loaded context window)
    int context_train = 0;  // n_ctx_train (native max)
};

// Fetch all model entries with context info from the configured server.
// Returns empty on error (including a protocol without a listing endpoint).
std::vector<ModelInfo> list_model_info(const Config& cfg);
std::vector<ModelInfo> list_model_info(const Config& cfg, const Dialect& dialect);

// Fetch all model IDs from the configured server. Returns empty on error.
std::vector<std::string> list_models(const Config& cfg);

// Fetch the kilo.ai account balance in USD via
// GET https://api.kilo.ai/api/profile/balance with `token` as the Bearer
// credential (the account OAuth token — anonymous gateway keys have no
// balance). Returns the balance on success, or a negative value on any
// transport/HTTP/parse failure so callers can hide the readout.
double fetch_kilo_balance(const std::string& token);

// Resolve the token the balance readout should use: the explicit
// kilo_balance_token override when set, else the provider's api_key when the
// active provider is kilocode (its gateway key IS the account token — models
// only work with a valid one, and the TUI key prompt stores it as api_key).
// Empty when no usable token exists (other providers, anonymous use).
std::string resolve_kilo_balance_token(const Config& cfg);

} // namespace agent

#endif // AGENT_MODEL_PROBE_H
