// The rate governor is the control law, tested with a supplied clock: no
// sleeps, no network. These pin the behavior the agent relies on — full blast
// until rejected, a one-off Retry-After pause, AIMD only for sustained
// rejection, fast recovery, provider isolation and published-limit budgets.

#include "agent/rate_governor.h"
#include "tests/test_util.h"

#include <chrono>

using namespace std::chrono_literals;

namespace {

using agent::RateClock;
using agent::RateGovernor;
using agent::RateHeaders;

const RateClock::time_point kStart{};

} // namespace

TEST(rate_governor_cold_start_is_full_blast) {
    RateGovernor g;
    ASSERT_EQ(g.wait_before(kStart, 260000).count(), 0);
    g.on_start(kStart);
    g.on_success(kStart + 1s, 260000);
    ASSERT_EQ(g.wait_before(kStart + 1s + 1ms, 260000).count(), 0);
    const auto status = g.status(kStart + 1s);
    ASSERT_EQ(status.interval_ms, 0L);
    ASSERT(status.concurrency > 4);
}

TEST(rate_governor_first_429_is_a_pause_not_an_interval) {
    RateGovernor g;
    g.on_start(kStart);
    g.on_rate_limit(kStart, 4s, {}, 0ms, 260000);

    const auto after = g.status(kStart);
    ASSERT_EQ(after.interval_ms, 0L); // one-off pause, no permanent tax
    ASSERT_EQ(after.concurrency, 4);
    ASSERT_EQ(after.pause_ms, 4000L);
    ASSERT_EQ(g.wait_before(kStart + 1s, 1000).count(), 3000L);
    ASSERT_EQ(g.wait_before(kStart + 4s, 1000).count(), 0L);

    g.on_start(kStart + 4s);
    g.on_success(kStart + 4s + 100ms, 1000);
    const auto recovered = g.status(kStart + 4s + 100ms);
    ASSERT_EQ(recovered.interval_ms, 0L);
    ASSERT(recovered.concurrency > 4);
}

TEST(rate_governor_repeated_429s_raise_interval_and_lower_concurrency) {
    RateGovernor g;
    g.on_start(kStart);
    g.on_rate_limit(kStart, 4s, {}, 0ms, 1000);
    g.on_start(kStart + 4s);
    g.on_rate_limit(kStart + 4s, 4s, {}, 0ms, 1000);

    const auto congested = g.status(kStart + 8s);
    ASSERT_EQ(congested.interval_ms, 250L);
    ASSERT_EQ(congested.concurrency, 2);
    ASSERT_EQ(congested.consecutive_rate_limits, 2);

    g.on_success(kStart + 8s, 1000);
    const auto recovering = g.status(kStart + 8s);
    ASSERT_EQ(recovering.interval_ms, 0L);
    ASSERT_EQ(recovering.concurrency, 3);
}

TEST(rate_governor_idle_resets_the_learned_penalty) {
    RateGovernor g;
    g.on_start(kStart);
    g.on_rate_limit(kStart, 1s, {}, 0ms, 1000);
    g.on_start(kStart + 1s);
    g.on_rate_limit(kStart + 1s, 1s, {}, 0ms, 1000);
    ASSERT_EQ(g.status(kStart + 2s).interval_ms, 250L);

    // The server pause is over; an idle minute is evidence of a fresh window.
    ASSERT_EQ(g.wait_before(kStart + 70s, 1000).count(), 0L);
    const auto idle = g.status(kStart + 70s);
    ASSERT_EQ(idle.interval_ms, 0L);
    ASSERT_EQ(idle.concurrency, 4);
}

TEST(rate_governor_uses_published_token_budget_then_refills) {
    RateGovernor g;
    RateHeaders h;
    h.token_limit = 100000;
    h.token_remaining = 50000;
    h.token_reset_ms = 4000;
    g.on_start(kStart);
    g.on_success(kStart, 1000, h);

    // 60k more does not fit in the 50k remaining; the reset is the wait.
    ASSERT(g.wait_before(kStart + 1s, 60000).count() > 0);
    // After the published reset the budget is full again.
    ASSERT_EQ(g.wait_before(kStart + 4s, 60000).count(), 0L);
    ASSERT_EQ(g.wait_before(kStart + 4s, 120000).count() > 0, true); // still too big alone
}

TEST(rate_governor_manual_cap_is_a_ceiling_with_a_ttl) {
    RateGovernor g;
    g.on_start(kStart);
    g.set_manual(kStart, 1s, 1, 0, 5s);
    ASSERT(g.wait_before(kStart + 1ms, 1).count() > 900);
    ASSERT_EQ(g.status(kStart).concurrency, 1);

    // Expired: the automatic policy is back, at full speed.
    ASSERT_EQ(g.wait_before(kStart + 6s, 1).count(), 0L);
    ASSERT_EQ(g.status(kStart + 6s).manual, false);
}

TEST(rate_governor_is_isolated_per_provider) {
    agent::Config a;
    a.provider_name = "alpha";
    a.api_base = "https://alpha.example/v1";
    a.api_key = "ka";
    agent::Config b;
    b.provider_name = "beta";
    b.api_base = "https://beta.example/v1";
    b.api_key = "kb";
    auto& ga = agent::RateGovernorRegistry::global().for_config(a);
    auto& gb = agent::RateGovernorRegistry::global().for_config(b);
    ASSERT(&ga != &gb);

    ga.on_response(429, 4000, {}, 1000);
    ASSERT(ga.status().pause_ms > 3000);
    ASSERT_EQ(gb.status().pause_ms, 0L);
}

TEST(rate_governor_parses_provider_durations_and_headers) {
    ASSERT_EQ(agent::parse_rate_duration_ms("4s"), 4000L);
    ASSERT_EQ(agent::parse_rate_duration_ms("20ms"), 20L);
    ASSERT_EQ(agent::parse_rate_duration_ms("6m0s"), 360000L);
    ASSERT_EQ(agent::parse_rate_duration_ms("1.5s"), 1500L);
    ASSERT_EQ(agent::parse_rate_duration_ms("30"), 30000L);
    ASSERT_EQ(agent::parse_rate_duration_ms("bogus"), 0L);

    RateHeaders h;
    agent::capture_rate_header("Anthropic-Ratelimit-Tokens-Remaining", "123", h);
    agent::capture_rate_header("x-ratelimit-limit-tokens", "100000", h);
    agent::capture_rate_header("x-ratelimit-reset-tokens", "4s", h);
    ASSERT_EQ(h.token_remaining, 123L);
    ASSERT_EQ(h.token_limit, 100000L);
    ASSERT_EQ(h.token_reset_ms, 4000L);
}

TEST(provider_governor_blocks_for_retry_after_and_cancels) {
    agent::ProviderGovernor g;
    agent::CancellationToken token;
    g.on_response(429, 40, {}, 100);
    const auto begin = RateClock::now();
    ASSERT(g.acquire(1, token));
    const auto waited =
        std::chrono::duration_cast<std::chrono::milliseconds>(RateClock::now() - begin);
    ASSERT(waited.count() >= 30);

    agent::CancellationToken cancelled;
    cancelled.request();
    ASSERT(!g.acquire(1, cancelled));
}
