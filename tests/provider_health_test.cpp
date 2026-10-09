#include "agent/provider_health.h"
#include "tests/test_util.h"

#include <string>

namespace {

// Config has defaulted fields, not a positional constructor: name the fields
// the probe reads so a test cannot silently probe the wrong endpoint.
agent::Config endpoint(const char* api_base, const char* flavor = "openai") {
    agent::Config c;
    c.api_base = api_base;
    c.flavor = flavor;
    c.api_key = "sk-test-token";
    return c;
}

} // namespace

// The outcome of one GET /models against a provider.
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

// The probe's classification, driven by an injected result rather than a
// network. The outcome is returned to the caller that asked -- the row of
// /get provider list -- and is never cached, so it cannot outlive the token it
// describes.

namespace {

agent::ProbeResult answering(long code) {
    return agent::ProbeResult{code, true};
}

const agent::ProbeResult kNoResponse{0, false};

} // namespace

TEST(auth_probe_classifies_a_2xx_as_valid) {
    const agent::Config cfg = endpoint("https://probe-valid.example/v1");
    const auto s =
        agent::auth_probe_blocking(cfg, [](const agent::Config&) { return answering(200); });
    ASSERT(s.state == agent::AuthState::Valid);
    ASSERT(s.http_code == 200);
}

TEST(auth_probe_classifies_a_401_as_rejected) {
    const agent::Config cfg = endpoint("https://probe-rejected.example/v1");
    const auto s =
        agent::auth_probe_blocking(cfg, [](const agent::Config&) { return answering(401); });
    ASSERT(s.state == agent::AuthState::Rejected);
    ASSERT(s.http_code == 401);
}

TEST(auth_probe_leaves_a_500_unknown) {
    // A server error must not be reported as a broken credential: the token may
    // be fine and the server may be down.
    const agent::Config cfg = endpoint("https://probe-500.example/v1");
    const auto s =
        agent::auth_probe_blocking(cfg, [](const agent::Config&) { return answering(500); });
    ASSERT(s.state == agent::AuthState::Unknown);
}

TEST(auth_probe_leaves_a_transport_failure_unknown) {
    const agent::Config cfg = endpoint("https://probe-refused.example/v1");
    const auto s =
        agent::auth_probe_blocking(cfg, [](const agent::Config&) { return kNoResponse; });
    ASSERT(s.state == agent::AuthState::Unknown);
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
    // Nothing to ask, so nothing is probed -- and the injected probe must not
    // run, which the call counter proves.
    const agent::Config cfg = endpoint("");
    g_probe_calls = 0;
    const auto s = agent::auth_probe_blocking(cfg, counting_probe);
    ASSERT(g_probe_calls == 0);
    ASSERT(s.state == agent::AuthState::Unknown);
}

// The checkbox column. These are the three states /get provider list promises:
// verified, configured-but-not-working, and not configured at all.

TEST(provider_mark_for_a_valid_token_is_a_tick) {
    ASSERT(agent::provider_mark(agent::AuthState::Valid, true) == 'x');
}

TEST(provider_mark_for_a_rejected_token_is_a_bang) {
    // Configured and keyed, but the server refused it.
    ASSERT(agent::provider_mark(agent::AuthState::Rejected, true) == '!');
}

TEST(provider_mark_for_an_unanswered_probe_is_a_bang) {
    // The probe could not tell (no answer, or a 5xx): configured, but not
    // working as far as this run knows, which is exactly what [!] means.
    ASSERT(agent::provider_mark(agent::AuthState::Unknown, true) == '!');
}

TEST(provider_mark_for_an_unconfigured_provider_is_blank) {
    // No endpoint, or a required key that is absent: nothing to verify, so
    // nothing is claimed -- whatever a previous probe may have said.
    ASSERT(agent::provider_mark(agent::AuthState::Unknown, false) == ' ');
    ASSERT(agent::provider_mark(agent::AuthState::Valid, false) == ' ');
}
