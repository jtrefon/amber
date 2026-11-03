#ifndef AGENT_PROVIDER_HEALTH_H
#define AGENT_PROVIDER_HEALTH_H

#include "agent/config.h"
#include "agent/model_probe.h"

#include <functional>
#include <string>

namespace agent {

class Dialect;

// Whether a provider's credential was accepted, as far as one probe could tell.
//
// The distinction this type exists to enforce: `Rejected` means the SERVER said
// the credential was bad, in whatever shape its protocol uses (an HTTP auth
// status, or a body marker such as Gemini's 400 API_KEY_INVALID). It never means
// "the request failed". A timeout, a refused connection, a DNS failure, a 404, a
// 429, a 5xx are all `Unknown`, because reporting a working token as broken
// after a network blip is worse than admitting the probe could not tell.
enum class AuthState {
    Unknown,  // no answer, or the answer was inconclusive
    Valid,    // the endpoint answered 2xx to an authenticated GET /models
    Rejected, // the endpoint refused the credential
};

// Classify one probe outcome from its HTTP status alone. `transport_ok` is
// whether an HTTP response was actually received; `http_code` is meaningless
// (and ignored) when it is false. Protocols whose auth failure does not use
// 401/403 go through auth_state_from_probe(), which asks the dialect.
AuthState auth_state_from_http(long http_code, bool transport_ok);

// What one probe attempt observed: the HTTP status, whether a response was
// received at all, and the body when it was buffered. `transport_ok == false`
// means the server never spoke.
struct ProbeResult {
    long http_code = 0;
    bool transport_ok = false;
    std::string body;
};

// Classify one probe with the protocol's own auth rule: the status, the body it
// may read, and the dialect that knows how this endpoint reports a bad key.
AuthState auth_state_from_probe(const ProbeResult& result, const Dialect& dialect);

// Perform one authenticated GET /models and report what came back. The only
// part of the probe that is not pure; injectable so the classification above it
// is testable without a network.
using ProbeFn = ProbeResult (*)(const Config&);

// One classified probe outcome. Nothing is cached: /get provider list runs the
// probe when it is asked and settles each row with the answer, so a verdict can
// never outlive the token it described.
struct AuthStatus {
    AuthState state = AuthState::Unknown;
    long http_code = 0;
};

// Blocking probe: run `probe` (the real GET /models when null) and classify the
// result. For worker threads only -- it blocks on the network.
AuthStatus auth_probe_blocking(const Config& cfg, ProbeFn probe = nullptr);

// Non-blocking probe for the UI thread: runs auth_probe_blocking on a detached
// worker and delivers the result through `post` (the host's UI-thread queue).
void auth_probe_async(const Config& cfg, std::function<void(std::function<void()>)> post,
                      std::function<void(const AuthStatus&)> done);

// What one credential check of a connection the user just typed observed.
//
// `auth_rejected` is the only verdict that must keep the user in the form: the
// server said the key is bad, so saving it would guarantee the next request
// fails. An unreachable endpoint or a 5xx is reported in `reason` but does not
// block a save -- a provider behind a VPN, or a service that is down, is still
// configurable, and "could not check" is not "invalid".
struct ConnectionCheck {
    bool auth_rejected = false;
    std::string reason; // empty when the endpoint accepted the credential
};

// Classify one GET /models for a connection the user just typed. Pure: the
// network call is models_get().
ConnectionCheck classify_connection(const ModelsGetResult& result, const Dialect& dialect);

// One bounded authenticated GET /models with the connection exactly as typed,
// for the provider form's OK path. Never writes the catalogue cache: validation
// is not a fetch. Blocking; callers run it behind the modal that asked.
ConnectionCheck validate_connection(const Config& cfg);

// The checkbox column in /get provider list:
//   'x' configured, and the endpoint accepted the token
//   '!' configured, but the probe did not confirm it: the token was rejected,
//       or no answer came back
//   ' ' not configured: no endpoint, or a required key that is absent, so there
//       is nothing to verify
//
// `configured` is the caller's verdict on the definition (an endpoint exists
// and, when one is required, a key is present). The mark never claims a token
// works without a probe that said so.
char provider_mark(AuthState state, bool configured);

} // namespace agent

#endif // AGENT_PROVIDER_HEALTH_H
