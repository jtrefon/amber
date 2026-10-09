#ifndef AGENT_PROVIDER_HEALTH_H
#define AGENT_PROVIDER_HEALTH_H

#include "agent/config.h"

#include <functional>
#include <string>

namespace agent {

// Whether a provider's credential was accepted, as far as one probe could tell.
//
// The distinction this type exists to enforce: `Rejected` means the SERVER said
// the credential was bad (401/403). It never means "the request failed". A
// timeout, a refused connection, a DNS failure, a 404, a 429, a 5xx are all
// `Unknown`, because reporting a working token as broken after a network blip
// is worse than admitting the probe could not tell.
enum class AuthState {
    Unknown,  // no answer, or the answer was inconclusive
    Valid,    // the endpoint answered 2xx to an authenticated GET /models
    Rejected, // the endpoint answered 401 or 403
};

// Classify one probe outcome. `transport_ok` is whether an HTTP response was
// actually received; `http_code` is meaningless (and ignored) when it is false.
AuthState auth_state_from_http(long http_code, bool transport_ok);

// What one probe attempt observed: the HTTP status, and whether a response was
// received at all. `transport_ok == false` means the server never spoke.
struct ProbeResult {
    long http_code = 0;
    bool transport_ok = false;
};

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
