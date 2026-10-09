#include "agent/provider_health.h"

#include "agent/model_probe.h"

#include <thread>

namespace agent {

AuthState auth_state_from_http(long http_code, bool transport_ok) {
    // No response was received, so no server has rejected anything.
    if (!transport_ok)
        return AuthState::Unknown;
    if (http_code == 401 || http_code == 403)
        return AuthState::Rejected;
    if (http_code >= 200 && http_code < 300)
        return AuthState::Valid;
    return AuthState::Unknown;
}

namespace {

// The real probe: the shared GET /models with its body discarded, so the status
// arrives intact. Reusing the catalogue's request keeps one implementation of
// "how do we call a models endpoint" rather than two that can drift.
ProbeResult probe_over_http(const Config& cfg) {
    const auto r = models_get(cfg, /*want_body=*/false);
    return ProbeResult{r.http_code, r.transport_ok};
}

} // namespace

AuthStatus auth_probe_blocking(const Config& cfg, ProbeFn probe) {
    AuthStatus status;
    if (cfg.api_base.empty())
        return status;

    const ProbeResult r = probe ? probe(cfg) : probe_over_http(cfg);
    status.http_code = r.http_code;
    status.state = auth_state_from_http(r.http_code, r.transport_ok);
    return status;
}

void auth_probe_async(const Config& cfg, std::function<void(std::function<void()>)> post,
                      std::function<void(const AuthStatus&)> done) {
    Config copy = cfg;
    std::thread([copy = std::move(copy), post = std::move(post), done = std::move(done)]() mutable {
        const AuthStatus s = auth_probe_blocking(copy);
        post([done = std::move(done), s] { done(s); });
    }).detach();
}

char provider_mark(AuthState state, bool configured) {
    if (!configured)
        return ' ';
    return state == AuthState::Valid ? 'x' : '!';
}

} // namespace agent
