// The cache gauge model: a rolling window, not a session average. These tests
// use injected timestamps so a day-long gap and an eight-call burst are one
// function call, not a sleeping test.

#include "plugins/cache/cache_reuse.h"
#include "plugins/cache/cache_text.h"

#include "agent/llm.h"
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

TEST(cache_reuse_call_hit_sub_one_stops_at_the_one_percent_boundary) {
    // 1 hit in 100 calls is exactly one percent: `call_hit_sub_one` is the
    // "below one percent" flag, so this is the boundary the floor still shows
    // as 1%. 1 hit in 101 calls is below it. A mutation of the comparison to
    // `<=` would flag the exact 1% case and read as a decreasing number.
    CacheWindow window;
    for (int i = 0; i < 100; ++i)
        window.record({at(i), 10, i == 0 ? 1 : 0, 0, "model", "provider", ""}, 100);
    const Readout exact = window.view(at(100), Metric::Calls, 100, kTtl);
    ASSERT_EQ(exact.hit_calls, 1);
    ASSERT_EQ(exact.call_hit_percent, 1);
    ASSERT_EQ(exact.call_hit_sub_one, false);

    window.record({at(100), 10, 0, 0, "model", "provider", ""}, 101);
    const Readout below = window.view(at(101), Metric::Calls, 101, kTtl);
    ASSERT_EQ(below.hit_calls, 1);
    ASSERT_EQ(below.call_hit_percent, 0);
    ASSERT_EQ(below.call_hit_sub_one, true);
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

TEST(cache_reuse_ignores_a_call_without_a_prompt_count) {
    CacheWindow window;
    ASSERT(window.record({at(0), 0, 10, 0, "model", "provider", ""}, 8) ==
           CacheWindow::Record::Ignored);
    ASSERT(window.view(at(0), Metric::Tokens, 8, kTtl).state == State::NoData);
}

TEST(cache_reuse_reset_forgets_everything) {
    CacheWindow window;
    window.record({at(0), 100, 80, 0, "model", "provider", "previous_message"}, 8);
    window.reset();
    const Readout r = window.view(at(0), Metric::Tokens, 8, kTtl);
    ASSERT(r.state == State::NoData);
    ASSERT(r.model.empty());
    ASSERT(r.provider.empty());
}

TEST(cache_reuse_stops_at_samples_older_than_the_ttl) {
    CacheWindow window;
    window.record({at(0), 100, 100, 0, "model", "provider", ""}, 8);
    window.record({at(400), 100, 50, 0, "model", "provider", ""}, 8);

    // The newest is fresh, so the gauge is live; the sample behind it is past
    // the TTL and must not dilute or extend the window.
    const Readout r = window.view(at(400), Metric::Tokens, 8, kTtl);
    ASSERT_EQ(r.calls, 1);
    ASSERT_EQ(r.percent, 50);
}

TEST(cache_reuse_clock_skew_never_yields_a_negative_age) {
    CacheWindow window;
    window.record({at(100), 100, 100, 0, "model", "provider", ""}, 8);
    const Readout r = window.view(at(50), Metric::Tokens, 8, kTtl);
    ASSERT_EQ(r.age_seconds, 0);
    ASSERT(r.state == State::Reuse);
}

TEST(cache_text_percent_age_and_bar) {
    Readout readout;
    readout.percent = 87;
    ASSERT_EQ(percent_text(readout), std::string("87%"));
    readout.sub_one = true;
    ASSERT_EQ(percent_text(readout), std::string("<1%"));

    ASSERT_EQ(age_text(-1), std::string("0s"));
    ASSERT_EQ(age_text(0), std::string("0s"));
    ASSERT_EQ(age_text(59), std::string("59s"));
    ASSERT_EQ(age_text(60), std::string("1m"));
    ASSERT_EQ(age_text(3599), std::string("59m"));
    ASSERT_EQ(age_text(3600), std::string("1h"));
    ASSERT_EQ(age_text(7322), std::string("2h"));

    Readout bar;
    ASSERT_TRUE(bar_text(bar).empty());
    bar.state = State::Unsupported;
    ASSERT_TRUE(bar_text(bar).empty());
    bar.state = State::Cold;
    ASSERT_EQ(bar_text(bar), std::string("  cache cold"));
    bar.state = State::Warm;
    ASSERT_EQ(bar_text(bar), std::string("  cache warm"));
    bar.state = State::Reuse;
    bar.percent = 0;
    ASSERT_EQ(bar_text(bar), std::string("  cache 0%"));
    bar.percent = 42;
    ASSERT_EQ(bar_text(bar), std::string("  cache 42%"));
    bar.percent = 100;
    ASSERT_EQ(bar_text(bar), std::string("  cache 100%"));

    // A measured 0/0 under the model's minimum is cold, not 0%.
    Readout tiny;
    tiny.state = State::Reuse;
    tiny.model = "claude-sonnet-4";
    tiny.input_tokens = 100;
    ASSERT_EQ(bar_text(tiny), std::string("  cache cold"));
}

TEST(cache_text_state_messages) {
    Readout readout;
    ASSERT_EQ(state_text(readout, Metric::Tokens, "5m"),
              std::string("cache: no completed requests yet"));

    readout.state = State::Unsupported;
    ASSERT_EQ(state_text(readout, Metric::Tokens, "5m"),
              std::string("cache: this provider reports no cache counters"));

    readout.state = State::Cold;
    readout.age_seconds = 5;
    ASSERT_EQ(state_text(readout, Metric::Tokens, "5m"),
              std::string("cache cold · last request 5s ago (TTL 5m)"));

    readout.state = State::Warm;
    readout.write_tokens = 1500;
    ASSERT_EQ(state_text(readout, Metric::Tokens, "5m"),
              std::string("cache warm · wrote 1.5k tokens, no read yet"));

    const int minimum = agent::min_cacheable_tokens("claude-sonnet-4");
    ASSERT(minimum > 0);
    readout.state = State::Reuse;
    readout.write_tokens = 0;
    readout.model = "claude-sonnet-4";
    readout.input_tokens = 100;
    ASSERT_EQ(state_text(readout, Metric::Tokens, "5m"),
              std::string("cache cold · prompt below the " + std::to_string(minimum) +
                          "-token minimum for claude-sonnet-4"));

    // Exactly at the minimum is a cacheable prompt, not a below-minimum one.
    // The `<` here is load-bearing: `<=` would call the exact boundary a miss
    // and print a cold bar for a request the provider can cache.
    readout.input_tokens = minimum;
    ASSERT_EQ(state_text(readout, Metric::Tokens, "5m").find("below the"), std::string::npos);
    ASSERT_EQ(bar_text(readout), std::string("  cache 0%"));
}

TEST(cache_text_reuse_detail_carries_every_field) {
    Readout readout;
    readout.state = State::Reuse;
    readout.percent = 87;
    readout.calls = 8;
    readout.hit_calls = 7;
    readout.call_hit_percent = 87;
    readout.input_tokens = 1000;
    readout.read_tokens = 870;
    readout.write_tokens = 30;
    readout.age_seconds = 90;
    readout.model = "claude-sonnet-4";
    readout.provider = "anthropic";
    readout.miss_reason = "previous_message";

    const std::string text = state_text(readout, Metric::Tokens, "1h");
    ASSERT(text.find("cache 87% · token reuse · last 8 calls · window 1h") != std::string::npos);
    ASSERT(text.find("calls hit: 7/8 (87%)") != std::string::npos);
    ASSERT(text.find("input 1k · read 870 · wrote 30") != std::string::npos);
    ASSERT(text.find("model anthropic/claude-sonnet-4") != std::string::npos);
    ASSERT(text.find("last request 1m ago") != std::string::npos);
    ASSERT(text.find("last miss: previous_message") != std::string::npos);

    // The calls metric changes the headline; provider-less models still name
    // the model, and a sample without a miss reason stays quiet about it.
    readout.provider.clear();
    readout.miss_reason.clear();
    const std::string calls = state_text(readout, Metric::Calls, "5m");
    ASSERT(calls.find("cache 87% · call hit · last 8 calls · window 5m") != std::string::npos);
    ASSERT(calls.find("model claude-sonnet-4") != std::string::npos);
    ASSERT(calls.find("last miss") == std::string::npos);

    // A sub-one-percent call hit rate reads "<1%", never a false "0%".
    readout.hit_calls = 1;
    readout.calls = 200;
    readout.call_hit_percent = 0;
    readout.call_hit_sub_one = true;
    ASSERT(state_text(readout, Metric::Calls, "5m").find("calls hit: 1/200 (<1%)") !=
           std::string::npos);
}
