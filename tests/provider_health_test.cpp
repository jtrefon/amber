#include "agent/provider_health.h"
#include "tests/test_util.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

// Config has defaulted fields, not a positional constructor: name the three the
// cache key depends on so a test cannot silently key off the wrong endpoint.
agent::Config endpoint(const char* api_base, const char* flavor = "openai") {
    agent::Config c;
    c.api_base = api_base;
    c.flavor = flavor;
    c.api_key = "sk-test-token";
    return c;
}

void write_text(const std::string& path, const std::string& body) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream f(path, std::ios::trunc);
    f << body;
}

} // namespace

// The recorded outcome of one GET /models against a provider.
//
// The rule the UI depends on: a token counts as "rejected" ONLY when the server
// said so with an auth status. Everything else -- a timeout, a refused
// connection, a DNS failure, a 500, a rate limit -- stays unknown, because
// reporting a working token as invalid because the network blipped is worse
// than reporting nothing.

TEST(auth_state_from_http_treats_2xx_as_a_valid_token) {
    ASSERT(agent::auth_state_from_http(200, true) == agent::AuthState::Valid);
    ASSERT(agent::auth_state_from_http(204, true) == agent::AuthState::Valid);
}

TEST(auth_state_from_http_marks_401_and_403_as_rejected) {
    ASSERT(agent::auth_state_from_http(401, true) == agent::AuthState::Rejected);
    ASSERT(agent::auth_state_from_http(403, true) == agent::AuthState::Rejected);
}

TEST(auth_state_from_http_does_not_call_other_failures_rejected) {
    // 404 is the wrong path rather than a bad token; 429 is a rate limit the
    // token did not cause; 5xx is the server's problem.
    ASSERT(agent::auth_state_from_http(404, true) == agent::AuthState::Unknown);
    ASSERT(agent::auth_state_from_http(429, true) == agent::AuthState::Unknown);
    ASSERT(agent::auth_state_from_http(500, true) == agent::AuthState::Unknown);
    ASSERT(agent::auth_state_from_http(503, true) == agent::AuthState::Unknown);
}

TEST(auth_state_from_http_keeps_a_transport_failure_unknown) {
    // No response at all: the server never spoke, so it never rejected anything.
    ASSERT(agent::auth_state_from_http(0, false) == agent::AuthState::Unknown);
    ASSERT(agent::auth_state_from_http(401, false) == agent::AuthState::Unknown);
}

// A verdict must survive a restart, or the checkbox would read unknown again
// the moment amber was closed -- which is the confusion this exists to remove.

TEST(auth_status_round_trips_through_the_cache) {
    const agent::Config cfg = endpoint("https://round-trip.example/v1");
    fs::remove(agent::auth_status_path(cfg));
    ASSERT(agent::auth_status_read(cfg).state == agent::AuthState::Unknown);

    agent::AuthStatus st;
    st.state = agent::AuthState::Rejected;
    st.http_code = 401;
    agent::auth_status_write(cfg, st);

    const auto back = agent::auth_status_read(cfg);
    ASSERT(back.state == agent::AuthState::Rejected);
    ASSERT(back.http_code == 401L);
    ASSERT(back.checked_ms > 0);
}

TEST(auth_status_survives_a_corrupt_cache_file) {
    const agent::Config cfg = endpoint("https://corrupt.example/v1");
    write_text(agent::auth_status_path(cfg), "{not json at all");
    ASSERT(agent::auth_status_read(cfg).state == agent::AuthState::Unknown);
}

// Two providers on one host must not share a verdict: different path, and the
// same path under a different wire protocol, are different endpoints.

TEST(auth_status_uses_a_different_file_per_endpoint) {
    const agent::Config a = endpoint("https://multi.example/v1");
    const agent::Config b = endpoint("https://multi.example/v2");
    const agent::Config c = endpoint("https://multi.example/v1", "anthropic");

    agent::AuthStatus ok;
    ok.state = agent::AuthState::Valid;
    ok.http_code = 200;
    agent::auth_status_write(a, ok);

    ASSERT(agent::auth_status_path(a) != agent::auth_status_path(b));
    ASSERT(agent::auth_status_path(a) != agent::auth_status_path(c));
    ASSERT(agent::auth_status_read(b).state == agent::AuthState::Unknown);
    ASSERT(agent::auth_status_read(c).state == agent::AuthState::Unknown);
}

// A stale verdict must not outlive the token it described: the user fixes the
// key, and the old "rejected" is the last thing that should still be shown.

TEST(auth_status_is_stale_past_the_ttl) {
    agent::AuthStatus old;
    old.state = agent::AuthState::Rejected;
    old.http_code = 401;
    old.checked_ms = agent::auth_status_now_ms() - 10LL * 3600 * 1000; // 10 hours ago
    ASSERT(!agent::auth_status_fresh(old));

    agent::AuthStatus recent = old;
    recent.checked_ms = agent::auth_status_now_ms() - 60LL * 1000; // a minute ago
    ASSERT(agent::auth_status_fresh(recent));
}

// The checkbox column. These are the three states /get provider list promises.

TEST(provider_mark_for_a_valid_token_is_a_tick) {
    ASSERT(agent::provider_mark(agent::AuthState::Valid, true, true) == 'x');
}

TEST(provider_mark_for_a_rejected_token_is_a_bang) {
    // An endpoint exists and the key is present, but the server refused it.
    ASSERT(agent::provider_mark(agent::AuthState::Rejected, true, true) == '!');
}

TEST(provider_mark_for_a_missing_key_is_a_bang_without_probing) {
    ASSERT(agent::provider_mark(agent::AuthState::Unknown, true, false) == '!');
}

TEST(provider_mark_for_an_unconfigured_provider_is_blank) {
    ASSERT(agent::provider_mark(agent::AuthState::Unknown, false, false) == ' ');
    ASSERT(agent::provider_mark(agent::AuthState::Valid, false, true) == ' ');
}

// The probe's classification and caching, driven by an injected result rather
// than a network. These are the paths that decide what the checkbox shows.

namespace {

agent::ProbeResult answering(long code) {
    return agent::ProbeResult{code, true};
}

const agent::ProbeResult kNoResponse{0, false};

} // namespace

TEST(auth_probe_records_a_2xx_as_valid) {
    const agent::Config cfg = endpoint("https://probe-valid.example/v1");
    const auto s = agent::auth_probe_blocking(cfg, [](const agent::Config&) {
        return answering(200);
    });
    ASSERT(s.state == agent::AuthState::Valid);
    ASSERT(s.http_code == 200);
    // And it is persisted, so it survives a restart.
    ASSERT(agent::auth_status_read(cfg).state == agent::AuthState::Valid);
}

TEST(auth_probe_records_a_401_as_rejected) {
    const agent::Config cfg = endpoint("https://probe-rejected.example/v1");
    const auto s = agent::auth_probe_blocking(cfg, [](const agent::Config&) {
        return answering(401);
    });
    ASSERT(s.state == agent::AuthState::Rejected);
    ASSERT(agent::auth_status_read(cfg).state == agent::AuthState::Rejected);
}

TEST(auth_probe_does_not_cache_an_inconclusive_result) {
    // The important negative: a 500 must not overwrite a verdict the user has
    // not invalidated. Their token may be fine and the server may be down.
    const agent::Config cfg = endpoint("https://probe-500.example/v1");
    agent::AuthStatus good;
    good.state = agent::AuthState::Rejected;
    good.http_code = 401;
    agent::auth_status_write(cfg, good);

    const auto s = agent::auth_probe_blocking(cfg, [](const agent::Config&) {
        return answering(500);
    });
    ASSERT(s.state == agent::AuthState::Unknown);
    // The previous verdict stands.
    ASSERT(agent::auth_status_read(cfg).state == agent::AuthState::Rejected);
}

TEST(auth_probe_does_not_cache_a_transport_failure) {
    const agent::Config cfg = endpoint("https://probe-refused.example/v1");
    agent::AuthStatus good;
    good.state = agent::AuthState::Valid;
    good.http_code = 200;
    agent::auth_status_write(cfg, good);

    const auto s = agent::auth_probe_blocking(cfg, [](const agent::Config&) { return kNoResponse; });
    ASSERT(s.state == agent::AuthState::Unknown);
    ASSERT(agent::auth_status_read(cfg).state == agent::AuthState::Valid);
}

// Probes are plain function pointers, so the call counter is file-static rather
// than a capture.
namespace {
int g_probe_calls = 0;

agent::ProbeResult counting_probe(const agent::Config&) {
    ++g_probe_calls;
    return answering(200);
}
} // namespace

TEST(auth_probe_skips_a_provider_with_no_endpoint) {
    // Nothing to ask, so nothing is probed and nothing is recorded -- and the
    // injected probe must not run, which the call counter proves.
    const agent::Config cfg = endpoint("");
    g_probe_calls = 0;
    const auto s = agent::auth_probe_blocking(cfg, counting_probe);
    ASSERT(g_probe_calls == 0);
    ASSERT(s.state == agent::AuthState::Unknown);
}

TEST(auth_probe_turns_a_rejection_into_the_bang_column) {
    // The whole chain the feature exists for, end to end.
    const agent::Config cfg = endpoint("https://probe-mark.example/v1");
    agent::auth_probe_blocking(cfg, [](const agent::Config&) { return answering(403); });
    const auto s = agent::auth_status_read(cfg);
    ASSERT(agent::provider_mark(s.state, true, true) == '!');
}
