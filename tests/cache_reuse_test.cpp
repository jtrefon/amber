// The cache gauge model: a rolling window, not a session average. These tests
// use injected timestamps so a day-long gap and an eight-call burst are one
// function call, not a sleeping test.

#include "plugins/cache/cache_reuse.h"

#include "test_util.h"

#include <chrono>

using namespace agent::plugins::cache;

namespace {

using Clock = std::chrono::steady_clock;

Clock::time_point at(int seconds) {
    return Clock::time_point(std::chrono::seconds(seconds));
}

constexpr std::chrono::seconds kTtl{300};

} // namespace

TEST(cache_reuse_token_weighted) {
    CacheWindow window;
    ASSERT(window.record({at(0), 100, 50, 0, "model", "provider", ""}, 8) ==
           CacheWindow::Record::Accepted);
    ASSERT(window.record({at(1), 900, 900, 0, "model", "provider", ""}, 8) ==
           CacheWindow::Record::Accepted);

    const Readout r = window.view(at(1), Metric::Tokens, 8, kTtl);
    ASSERT_EQ(r.state == State::Reuse, true);
    ASSERT_EQ(r.percent, 95);
    ASSERT_EQ(r.calls, 2);
    ASSERT_EQ(r.hit_calls, 2);
    ASSERT_EQ(r.input_tokens, 1000L);
    ASSERT_EQ(r.read_tokens, 950L);
}

TEST(cache_reuse_calls_metric_is_a_hit_rate) {
    CacheWindow window;
    window.record({at(0), 100, 100, 0, "model", "provider", ""}, 8);
    window.record({at(1), 100, 0, 0, "model", "provider", ""}, 8);
    window.record({at(2), 100, 0, 0, "model", "provider", ""}, 8);

    const Readout r = window.view(at(2), Metric::Calls, 8, kTtl);
    ASSERT_EQ(r.percent, 33);
    ASSERT_EQ(r.call_hit_percent, 33);
    ASSERT_EQ(r.calls, 3);
    ASSERT_EQ(r.hit_calls, 1);
    // Token reuse is still available in the detail.
    ASSERT_EQ(r.read_tokens, 100L);
    ASSERT_EQ(r.input_tokens, 300L);
}

TEST(cache_reuse_floors_and_never_fakes_a_full_window) {
    CacheWindow window;
    window.record({at(0), 1000, 3, 0, "model", "provider", ""}, 8);
    const Readout tiny = window.view(at(0), Metric::Tokens, 8, kTtl);
    ASSERT_EQ(tiny.percent, 0);
    ASSERT_EQ(tiny.sub_one, true);

    CacheWindow almost;
    almost.record({at(0), 1000, 999, 0, "model", "provider", ""}, 8);
    const Readout r = almost.view(at(0), Metric::Tokens, 8, kTtl);
    ASSERT_EQ(r.percent, 99);
    ASSERT_EQ(r.sub_one, false);

    CacheWindow full;
    full.record({at(0), 1000, 1000, 0, "model", "provider", ""}, 8);
    ASSERT_EQ(full.view(at(0), Metric::Tokens, 8, kTtl).percent, 100);
}

TEST(cache_reuse_ttl_makes_the_window_cold) {
    CacheWindow window;
    window.record({at(0), 100, 100, 0, "model", "provider", ""}, 8);

    ASSERT(window.view(at(300), Metric::Tokens, 8, kTtl).state == State::Reuse);
    const Readout cold = window.view(at(301), Metric::Tokens, 8, kTtl);
    ASSERT(cold.state == State::Cold);
    ASSERT_EQ(cold.age_seconds, 301);
}

TEST(cache_reuse_keeps_only_the_recent_window) {
    CacheWindow window;
    for (int i = 0; i < 4; ++i)
        window.record({at(i), 10, 0, 0, "model", "provider", ""}, 8);
    for (int i = 4; i < 12; ++i)
        window.record({at(i), 10, 10, 0, "model", "provider", ""}, 8);

    const Readout r = window.view(at(11), Metric::Tokens, 8, kTtl);
    ASSERT_EQ(r.percent, 100);
    ASSERT_EQ(r.calls, 8);
    ASSERT_EQ(r.hit_calls, 8);
}

TEST(cache_reuse_provider_switch_resets_the_window) {
    CacheWindow window;
    window.record({at(0), 100, 100, 0, "model-a", "provider-a", ""}, 8);
    window.record({at(1), 100, 0, 0, "model-b", "provider-b", ""}, 8);

    const Readout r = window.view(at(1), Metric::Tokens, 8, kTtl);
    ASSERT_EQ(r.percent, 0);
    ASSERT_EQ(r.calls, 1);
    ASSERT_EQ(r.model, std::string("model-b"));
    ASSERT_EQ(r.provider, std::string("provider-b"));
}

TEST(cache_reuse_unsupported_counters_hide_the_window) {
    CacheWindow window;
    ASSERT(window.record({at(0), 100, -1, -1, "model", "provider", ""}, 8) ==
           CacheWindow::Record::Unsupported);
    ASSERT(window.view(at(0), Metric::Tokens, 8, kTtl).state == State::Unsupported);

    ASSERT(window.record({at(1), 100, 80, 0, "model", "provider", ""}, 8) ==
           CacheWindow::Record::Accepted);
    ASSERT(window.view(at(1), Metric::Tokens, 8, kTtl).state == State::Reuse);
}

TEST(cache_reuse_write_only_is_warming_not_zero) {
    CacheWindow window;
    window.record({at(0), 1000, 0, 1000, "model", "provider", ""}, 8);
    const Readout r = window.view(at(0), Metric::Tokens, 8, kTtl);
    ASSERT(r.state == State::Warm);
    ASSERT_EQ(r.percent, 0);
    ASSERT_EQ(r.write_tokens, 1000L);
}

TEST(cache_reuse_empty_window_has_no_number) {
    CacheWindow window;
    ASSERT(window.view(at(0), Metric::Tokens, 8, kTtl).state == State::NoData);
}

TEST(cache_reuse_ttl_zero_is_cold) {
    CacheWindow window;
    window.record({at(0), 100, 100, 0, "model", "provider", ""}, 8);
    ASSERT(window.view(at(0), Metric::Tokens, 8, std::chrono::seconds(0)).state == State::Cold);
}
