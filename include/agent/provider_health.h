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
    Unknown,  // never probed, stale, or the answer was inconclusive
    Valid,    // the endpoint answered 2xx to an authenticated GET /models
    Rejected, // the endpoint answered 401 or 403
};

// Classify one probe outcome. `transport_ok` is whether an HTTP response was
// actually received; `http_code` is meaningless (and ignored) when it is false.
AuthState auth_state_from_http(long http_code, bool transport_ok);

// What a probe recorded, and when.
struct AuthStatus {
    AuthState state = AuthState::Unknown;
    long http_code = 0;
    long long checked_ms = 0;
};

// Cache location for cfg's endpoint verdict, under the global config dir.
// Per (api_base, flavor), matching the catalog cache: two providers on one host
// must not share a verdict, and neither may share one across wire protocols.
std::string auth_status_path(const Config& cfg);

// Disk-only read. Never touches the network. Returns Unknown for an absent,
// corrupt, or expired record -- a stale rejection must not outlive the token it
// described, or fixing the key would leave the old verdict showing.
AuthStatus auth_status_read(const Config& cfg);

// Persist a verdict (atomic tmp+rename).
void auth_status_write(const Config& cfg, const AuthStatus& status);

// Wall clock in ms, exposed so a caller can synthesise an expired record.
long long auth_status_now_ms();

// A verdict younger than the TTL is still shown.
bool auth_status_fresh(const AuthStatus& status);

// What one probe attempt observed: the HTTP status, and whether a response was
// received at all. `transport_ok == false` means the server never spoke.
struct ProbeResult {
    long http_code = 0;
    bool transport_ok = false;
};

// Perform one authenticated GET /models and report what came back. The only
// part of the probe that is not pure; injectable so the classification and
// caching above it are testable without a network.
using ProbeFn = ProbeResult (*)(const Config&);

// Blocking probe: run `probe` (the real GET /models when null), classify the
// result, and cache it. For worker threads only -- it blocks on the network.
// Returns the recorded status.
AuthStatus auth_probe_blocking(const Config& cfg, ProbeFn probe = nullptr);

// Non-blocking probe for the UI thread: runs auth_probe_blocking on a detached
// worker and delivers the result through `post` (the host's UI-thread queue).
void auth_probe_async(const Config& cfg, std::function<void(std::function<void()>)> post,
                      std::function<void(const AuthStatus&)> done);

// The checkbox column in /get provider list:
//   'x' configured and the server accepted the token
//   '!' configured but the key is missing, or the server rejected it
//   ' ' not configured
//
// has_endpoint false wins over everything: there is nothing to be valid.
char provider_mark(AuthState state, bool has_endpoint, bool has_key);

} // namespace agent

#endif // AGENT_PROVIDER_HEALTH_H
