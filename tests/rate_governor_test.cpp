// The rate governor is the control law, tested with a supplied clock: no
// sleeps, no network. These pin the behavior the agent relies on — full blast
// until rejected, a one-off Retry-After pause, AIMD only for sustained
// rejection, fast recovery, provider isolation and published-limit budgets.

#include "agent/model_probe.h"
#include "agent/rate_governor.h"
#include "tests/test_util.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

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

namespace {

using agent::ProviderGovernor;
using agent::RequestClass;

// Poll a condition with a short quantum: the governor's own wait loop wakes on
// a 200 ms safety timer, so a test that waits on it must not assume an instant
// wake. The limit is generous; a correct implementation returns in a few ms.
bool wait_until(const std::function<bool()>& predicate,
                std::chrono::milliseconds limit = std::chrono::milliseconds(2000)) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

// A failed assertion throws out of the test body; a joinable std::thread would
// then call std::terminate before the harness can report the failure. Joining
// in a destructor keeps the failure visible.
class ThreadJoiner {
public:
    explicit ThreadJoiner(std::thread& thread) : thread_(thread) {}
    ~ThreadJoiner() {
        if (thread_.joinable())
            thread_.join();
    }
    ThreadJoiner(const ThreadJoiner&) = delete;
    ThreadJoiner& operator=(const ThreadJoiner&) = delete;

private:
    std::thread& thread_;
};

int waiting_for(const agent::RateStatus& status, RequestClass cls) {
    switch (cls) {
    case RequestClass::Interactive:
        return status.waiting_interactive;
    case RequestClass::Foreground:
        return status.waiting_foreground;
    case RequestClass::Background:
        return status.waiting_background;
    }
    return 0;
}

} // namespace

// The thread-local class is what the transport reads. It defaults to
// Interactive, nests, and restores the outer class on scope exit.
TEST(request_class_scope_defaults_to_interactive_and_nests) {
    ASSERT(agent::current_request_class() == RequestClass::Interactive);
    {
        agent::ScopedRequestClass background(RequestClass::Background);
        ASSERT(agent::current_request_class() == RequestClass::Background);
        {
            agent::ScopedRequestClass foreground(RequestClass::Foreground);
            ASSERT(agent::current_request_class() == RequestClass::Foreground);
        }
        ASSERT(agent::current_request_class() == RequestClass::Background);
    }
    ASSERT(agent::current_request_class() == RequestClass::Interactive);
}

// A cold start admits every class; class is a policy about contention, not a
// second budget.
TEST(governor_background_runs_when_idle) {
    ProviderGovernor g;
    agent::CancellationToken token;
    ASSERT(g.acquire(1, token, RequestClass::Background));
    ASSERT_EQ(g.status().in_flight_background, 1);
    ASSERT_EQ(g.status().in_flight_interactive, 0);
    g.on_response(200, 0, {}, 1);
    ASSERT_EQ(g.status().in_flight_background, 0);
}

// The default call site is still the user's turn: a caller that never mentions
// a class is admitted as Interactive and shows up in that column.
TEST(governor_default_class_is_interactive) {
    ProviderGovernor g;
    agent::CancellationToken token;
    ASSERT(g.acquire(1, token));
    ASSERT_EQ(g.status().in_flight_interactive, 1);
    ASSERT_EQ(g.status().in_flight_foreground, 0);
    ASSERT_EQ(g.status().in_flight_background, 0);
    g.on_response(200, 0, {}, 1);
}

// One slot: an interactive waiter goes before a background waiter when the
// slot frees.
TEST(governor_interactive_precedes_background) {
    ProviderGovernor g;
    g.set_manual(0ms, 1, 0, 60s);
    agent::CancellationToken held;
    ASSERT(g.acquire(1, held, RequestClass::Interactive));

    std::atomic<int> order{0};
    std::atomic<int> interactive_order{0};
    std::atomic<int> background_order{0};
    std::atomic<bool> interactive_done{false};
    std::atomic<bool> background_done{false};

    std::thread background([&] {
        agent::CancellationToken token;
        const bool ok = g.acquire(1, token, RequestClass::Background);
        background_order = ++order;
        background_done = true;
        if (ok)
            g.on_response(200, 0, {}, 1);
    });
    ThreadJoiner join_background(background);
    ASSERT(wait_until([&] { return g.status().waiting_background == 1; }));

    std::thread interactive([&] {
        agent::CancellationToken token;
        const bool ok = g.acquire(1, token, RequestClass::Interactive);
        interactive_order = ++order;
        interactive_done = true;
        if (ok)
            g.on_response(200, 0, {}, 1);
    });
    ThreadJoiner join_interactive(interactive);
    ASSERT(wait_until([&] { return g.status().waiting_interactive == 1; }));

    g.on_response(200, 0, {}, 1); // release the held request
    ASSERT(wait_until([&] { return interactive_done.load(); }));
    ASSERT(wait_until([&] { return background_done.load(); }));
    ASSERT_EQ(interactive_order.load(), 1);
    ASSERT_EQ(background_order.load(), 2);
}

// An in-flight request is never preempted; a background caller waits for the
// next admission even when the base concurrency would have a free slot.
TEST(governor_background_does_not_preempt_inflight) {
    ProviderGovernor g;
    g.set_manual(0ms, 2, 0, 60s); // the class rule, not the slot, is the blocker
    agent::CancellationToken held;
    ASSERT(g.acquire(1, held, RequestClass::Interactive));

    std::atomic<bool> done{false};
    std::thread background([&] {
        agent::CancellationToken token;
        const bool ok = g.acquire(1, token, RequestClass::Background);
        done = true;
        if (ok)
            g.on_response(200, 0, {}, 1);
    });
    ThreadJoiner join_background(background);
    ASSERT(wait_until([&] { return g.status().waiting_background == 1; }));
    ASSERT_FALSE(wait_until([&] { return done.load(); }, 25ms));

    g.on_response(200, 0, {}, 1);
    ASSERT(wait_until([&] { return done.load(); }));
}

// A background waiter that has waited past the aging bound is promoted to
// Foreground for admission: it runs while a Foreground request is in flight,
// because only an Interactive request can block a Foreground admission.
TEST(governor_background_ages_into_foreground) {
    ProviderGovernor g;
    g.set_background_aging(30ms);
    g.set_manual(0ms, 2, 0, 60s);
    agent::CancellationToken held;
    ASSERT(g.acquire(1, held, RequestClass::Foreground));

    std::atomic<bool> done{false};
    std::thread background([&] {
        agent::CancellationToken token;
        const bool ok = g.acquire(1, token, RequestClass::Background);
        done = true;
        if (ok)
            g.on_response(200, 0, {}, 1);
    });
    ThreadJoiner join_background(background);
    ASSERT(wait_until([&] { return g.status().waiting_background == 1; }));
    ASSERT_FALSE(wait_until([&] { return done.load(); }, 15ms)); // not promoted yet
    ASSERT(wait_until([&] { return done.load(); }));             // the aging bound admits it

    g.on_response(200, 0, {}, 1);
}

// A 429 is provider-wide: the pause a background request learned blocks the
// user's next interactive request too.
TEST(governor_429_pauses_all_classes) {
    ProviderGovernor g;
    agent::CancellationToken token;
    g.on_response(429, 40, {}, 100);
    const auto begin = RateClock::now();
    ASSERT(g.acquire(1, token, RequestClass::Interactive));
    const auto interactive_wait =
        std::chrono::duration_cast<std::chrono::milliseconds>(RateClock::now() - begin);
    ASSERT(interactive_wait.count() >= 30);
    g.on_response(200, 0, {}, 1);

    g.on_response(429, 40, {}, 100);
    const auto begin_background = RateClock::now();
    ASSERT(g.acquire(1, token, RequestClass::Background));
    const auto background_wait =
        std::chrono::duration_cast<std::chrono::milliseconds>(RateClock::now() - begin_background);
    ASSERT(background_wait.count() >= 30);
    g.on_response(200, 0, {}, 1);
}

// One governor, one bucket: a success in one class advances the same AIMD
// state the next class reads.
TEST(governor_one_bucket_across_classes) {
    ProviderGovernor g;
    agent::CancellationToken token;
    ASSERT(g.acquire(1, token, RequestClass::Interactive));
    g.on_response(200, 0, {}, 1);
    const int after_interactive = g.status().concurrency;
    ASSERT(after_interactive > 4);

    ASSERT(g.acquire(1, token, RequestClass::Background));
    g.on_response(200, 0, {}, 1);
    const auto status = g.status();
    ASSERT(status.concurrency > after_interactive);
    ASSERT_EQ(status.successes, 2);
    ASSERT_EQ(status.interval_ms, 0L);
}

// Cancellation wakes and removes a waiter in every class, including one that
// was blocked only by the class above it.
TEST(governor_cancel_wakes_every_class) {
    for (const RequestClass cls :
         {RequestClass::Interactive, RequestClass::Foreground, RequestClass::Background}) {
        ProviderGovernor g;
        g.set_manual(0ms, 1, 0, 60s);
        agent::CancellationToken held;
        ASSERT(g.acquire(1, held, RequestClass::Interactive));

        agent::CancellationToken cancel;
        std::atomic<bool> done{false};
        std::atomic<bool> admitted{true};
        std::thread waiter([&] {
            admitted = g.acquire(1, cancel, cls);
            done = true;
        });
        ThreadJoiner join_waiter(waiter);
        ASSERT(wait_until([&] { return waiting_for(g.status(), cls) == 1; }));
        cancel.request();
        ASSERT(wait_until([&] { return done.load(); }));
        ASSERT_FALSE(admitted.load());
        g.on_response(200, 0, {}, 1);
    }
}

// /throttle can say who is queued: waiting and in-flight counts are reported
// per class, from the one governor every caller shares.
TEST(governor_status_reports_waiters_by_class) {
    ProviderGovernor g;
    g.set_manual(0ms, 1, 0, 60s);
    agent::CancellationToken held;
    ASSERT(g.acquire(1, held, RequestClass::Interactive));

    std::atomic<bool> interactive_done{false};
    std::atomic<bool> foreground_done{false};
    std::atomic<bool> background_done{false};
    std::thread interactive([&] {
        agent::CancellationToken token;
        if (g.acquire(1, token, RequestClass::Interactive))
            g.on_response(200, 0, {}, 1);
        interactive_done = true;
    });
    ThreadJoiner join_interactive(interactive);
    std::thread foreground([&] {
        agent::CancellationToken token;
        if (g.acquire(1, token, RequestClass::Foreground))
            g.on_response(200, 0, {}, 1);
        foreground_done = true;
    });
    ThreadJoiner join_foreground(foreground);
    std::thread background([&] {
        agent::CancellationToken token;
        if (g.acquire(1, token, RequestClass::Background))
            g.on_response(200, 0, {}, 1);
        background_done = true;
    });
    ThreadJoiner join_background(background);

    ASSERT(wait_until([&] {
        const auto status = g.status();
        return status.waiting_interactive == 1 && status.waiting_foreground == 1 &&
               status.waiting_background == 1 && status.in_flight_interactive == 1;
    }));

    g.on_response(200, 0, {}, 1);
    ASSERT(wait_until([&] {
        return interactive_done.load() && foreground_done.load() && background_done.load();
    }));
}

// The model catalogue is a governed Foreground request: a pause the provider
// taught a chat also holds the listing back. Port 1 refuses immediately, so
// the elapsed time is the governor's wait, not a network timeout.
TEST(model_catalog_request_shares_the_provider_pause) {
    agent::Config cfg;
    cfg.provider_name = "catalog-pause-test";
    cfg.api_base = "http://127.0.0.1:1/v1";
    cfg.flavor = "openai";
    auto& governor = agent::RateGovernorRegistry::global().for_config(cfg);
    governor.on_response(429, 40, {}, 0);

    const auto begin = RateClock::now();
    const auto result = agent::models_get(cfg, /*want_body=*/true);
    const auto waited =
        std::chrono::duration_cast<std::chrono::milliseconds>(RateClock::now() - begin);
    ASSERT(waited.count() >= 30);
    ASSERT_FALSE(result.transport_ok);
}

// A cancelled catalogue request returns before the GET; the governor did not
// admit it, so there is nothing to report back.
TEST(model_catalog_request_cancels_before_the_get) {
    agent::Config cfg;
    cfg.provider_name = "catalog-cancel-test";
    cfg.api_base = "http://127.0.0.1:1/v1";
    cfg.flavor = "openai";
    cfg.cancel_token.request();
    const auto result = agent::models_get(cfg, /*want_body=*/false);
    ASSERT_FALSE(result.transport_ok);
    ASSERT(result.body.empty());
}

// A Foreground request blocks on an interactive request that is already in
// flight, even though the class has no interactive *waiter*.
TEST(governor_foreground_waits_for_an_inflight_interactive) {
    ProviderGovernor g;
    g.set_manual(0ms, 2, 0, 60s);
    agent::CancellationToken held;
    ASSERT(g.acquire(1, held, RequestClass::Interactive));

    std::atomic<bool> done{false};
    std::thread foreground([&] {
        agent::CancellationToken token;
        const bool ok = g.acquire(1, token, RequestClass::Foreground);
        done = true;
        if (ok)
            g.on_response(200, 0, {}, 1);
    });
    ThreadJoiner join_foreground(foreground);
    ASSERT(wait_until([&] { return g.status().waiting_foreground == 1; }));
    ASSERT_FALSE(wait_until([&] { return done.load(); }, 25ms));

    g.on_response(200, 0, {}, 1);
    ASSERT(wait_until([&] { return done.load(); }));
}

// A Background request blocks on a Foreground request that is already in
// flight, not only on one that is waiting.
TEST(governor_background_waits_for_an_inflight_foreground) {
    ProviderGovernor g;
    g.set_manual(0ms, 2, 0, 60s);
    agent::CancellationToken held;
    ASSERT(g.acquire(1, held, RequestClass::Foreground));

    std::atomic<bool> done{false};
    std::thread background([&] {
        agent::CancellationToken token;
        const bool ok = g.acquire(1, token, RequestClass::Background);
        done = true;
        if (ok)
            g.on_response(200, 0, {}, 1);
    });
    ThreadJoiner join_background(background);
    ASSERT(wait_until([&] { return g.status().waiting_background == 1; }));
    ASSERT_FALSE(wait_until([&] { return done.load(); }, 25ms));

    g.on_response(200, 0, {}, 1);
    ASSERT(wait_until([&] { return done.load(); }));
}

// A zero aging bound is clamped to a positive quantum, not a busy spin.
TEST(governor_aging_bound_has_a_floor) {
    ProviderGovernor g;
    g.set_background_aging(0ms);
    agent::CancellationToken token;
    ASSERT(g.acquire(1, token, RequestClass::Background));
    g.on_response(200, 0, {}, 1);
}
