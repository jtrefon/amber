#include "agent/provider_health.h"

#include "agent/model_probe.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace agent {

namespace {

// A verdict older than this is dropped on read: the user fixes a key, and the
// old "rejected" must not still be showing an hour later.
constexpr long long kAuthTtlMs = 6LL * 3600 * 1000;

size_t discard_body(char*, size_t size, size_t nmemb, void*) {
    return size * nmemb;
}

} // namespace

AuthState auth_state_from_http(long http_code, bool transport_ok) {
    // No response was received, so no server has rejected anything. A status
    // carried over from a previous attempt must not be believed.
    if (!transport_ok)
        return AuthState::Unknown;
    if (http_code == 401 || http_code == 403)
        return AuthState::Rejected;
    if (http_code >= 200 && http_code < 300)
        return AuthState::Valid;
    return AuthState::Unknown;
}

long long auth_status_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string auth_status_path(const Config& cfg) {
    // Same FNV-1a key as the catalog cache, over the same identity, so a
    // provider's verdict and its catalogue can never be confused for one another.
    uint64_t h = 1469598103934665603ULL;
    const std::string key = cfg.api_base + "\n" + cfg.flavor;
    for (unsigned char c : key) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    char name[32];
    std::snprintf(name, sizeof(name), "auth-%016llx.json", static_cast<unsigned long long>(h));
    return global_config_dir() + "/cache/" + name;
}

bool auth_status_fresh(const AuthStatus& status) {
    return auth_status_now_ms() - status.checked_ms < kAuthTtlMs;
}

AuthStatus auth_status_read(const Config& cfg) {
    std::ifstream f(auth_status_path(cfg));
    if (!f)
        return {};
    json j;
    try {
        f >> j;
        AuthStatus s;
        s.http_code = j.value("http_code", 0L);
        s.checked_ms = j.value("checked_ms", 0LL);
        const std::string state = j.value("state", std::string("unknown"));
        s.state = state == "valid"      ? AuthState::Valid
                  : state == "rejected" ? AuthState::Rejected
                                        : AuthState::Unknown;
        // A stale record is worse than none: it would keep showing a rejection
        // for a key the user has since replaced.
        return auth_status_fresh(s) ? s : AuthStatus{};
    } catch (...) {
        return {};
    }
}

void auth_status_write(const Config& cfg, const AuthStatus& status) {
    const std::string path = auth_status_path(cfg);
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f)
            return;
        const char* name = status.state == AuthState::Valid      ? "valid"
                           : status.state == AuthState::Rejected ? "rejected"
                                                                 : "unknown";
        AuthStatus stamped = status;
        stamped.checked_ms = auth_status_now_ms();
        f << json{{"state", name},
                  {"http_code", stamped.http_code},
                  {"checked_ms", stamped.checked_ms}}
                 .dump();
        if (!f)
            return;
    }
    fs::rename(tmp, path, ec);
    if (ec)
        fs::remove(tmp, ec);
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
    status.checked_ms = auth_status_now_ms();
    // Only a conclusive answer is worth keeping: an inconclusive probe must not
    // overwrite a previous verdict with Unknown.
    if (status.state != AuthState::Unknown)
        auth_status_write(cfg, status);
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

char provider_mark(AuthState state, bool has_endpoint, bool has_key) {
    if (!has_endpoint)
        return ' ';
    if (!has_key)
        return '!';
    return state == AuthState::Valid ? 'x' : (state == AuthState::Rejected ? '!' : ' ');
}

} // namespace agent
