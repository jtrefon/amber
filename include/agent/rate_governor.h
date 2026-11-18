#ifndef AGENT_RATE_GOVERNOR_H
#define AGENT_RATE_GOVERNOR_H

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "agent/config.h"
#include "agent/process.h"

namespace agent {

using RateClock = std::chrono::steady_clock;

// What a request is for. The class does not create a second budget: one
// provider/account still has one token bucket and one concurrency limit. It
// only orders the waiters when that budget is contended, so a background
// worker cannot push the user's turn behind it.
enum class RequestClass : std::uint8_t {
    Interactive, // a user turn
    Foreground,  // compression, a model catalogue, a turn's own retries
    Background   // a worker, a watch summariser, a monitor observer
};

// The class the next governed request on this thread asks for. The transport
// reads it; the default is Interactive, so a host that never mentions a class
// keeps the old behaviour.
RequestClass current_request_class() noexcept;

// Set the class for the enclosing request path and restore the outer one on
// scope exit. One scope belongs to one request (or one compression pass);
// nesting is how a Foreground compression runs inside a Background worker.
class ScopedRequestClass {
public:
    explicit ScopedRequestClass(RequestClass cls) noexcept;
    ~ScopedRequestClass();
    ScopedRequestClass(const ScopedRequestClass&) = delete;
    ScopedRequestClass& operator=(const ScopedRequestClass&) = delete;

private:
    RequestClass previous_;
};

// Provider rate-limit headers, when the endpoint publishes them. Absent fields
// are 0/-1 ("unknown"), never a guess.
struct RateHeaders {
    long request_limit = 0;
    long request_remaining = -1;
    long request_reset_ms = 0;
    long token_limit = 0;
    long token_remaining = -1;
    long token_reset_ms = 0;
};

// What the UI and the model can read: the whole control state, not a byte.
struct RateStatus {
    long interval_ms = 0;
    long pause_ms = 0;
    int concurrency = 0;
    int in_flight = 0;
    int consecutive_rate_limits = 0;
    int successes = 0;
    long tokens_in_window = 0;
    long token_capacity = 0;
    long request_capacity = 0;
    long learned_tpm = 0;
    long learned_rpm = 0;
    bool manual = false;
    long manual_remaining_ms = 0;
    // Who is queued and who holds a slot, by class. `in_flight` above is the
    // base total; an aged background request is counted under the class it was
    // admitted as (Foreground), which is what /throttle should show.
    int waiting_interactive = 0;
    int waiting_foreground = 0;
    int waiting_background = 0;
    int in_flight_interactive = 0;
    int in_flight_foreground = 0;
    int in_flight_background = 0;
};

// Retry-After seconds -> ms. A date or garbage is 0 (the fallback is used).
long parse_retry_after_ms(const std::string& value);

// Provider reset headers are durations ("4s", "20ms", "6m0s") or bare seconds.
// 0 means unknown.
long parse_rate_duration_ms(const std::string& value);

// Fold one response header into `out`. Header names are case-insensitive.
void capture_rate_header(const std::string& name, const std::string& value, RateHeaders& out);

// The control law, with `now` supplied by the caller so a fake clock can drive
// every scenario without sleeping. It owns how much latency a request costs,
// never the request itself.
//
// Cold start is deliberate: interval 0 and the full concurrency, because no
// evidence says the provider is limited yet. The first 429 is a one-off pause
// (Retry-After); sustained rejection is what raises the interval and lowers
// concurrency (AIMD). A success is evidence of headroom and decays the penalty
// quickly; idle time resets it. Token budgets are enforced only when the
// provider publishes them.
class RateGovernor {
public:
    RateGovernor();

    // 0 means admit now. A positive value is the wait before the next probe.
    std::chrono::milliseconds wait_before(RateClock::time_point now, long tokens);

    void on_start(RateClock::time_point now);
    void on_success(RateClock::time_point now, long tokens, const RateHeaders& headers = {});
    // `retry_after` is the provider's header (0 when absent); `jitter` is
    // supplied by the caller so tests remain deterministic.
    void on_rate_limit(RateClock::time_point now, std::chrono::milliseconds retry_after,
                       const RateHeaders& headers, std::chrono::milliseconds jitter, long tokens);
    void on_failure(RateClock::time_point now);

    // A manual ceiling from the throttle tool. It expires after `ttl`; the
    // automatic policy never exceeds it while it is alive.
    void set_manual(RateClock::time_point now, std::chrono::milliseconds interval, int concurrency,
                    long token_cap, std::chrono::milliseconds ttl);
    void clear_manual();

    RateStatus status(RateClock::time_point now) const;
    // The wait the next retry would face, without admitting a request.
    std::chrono::milliseconds retry_wait(RateClock::time_point now) const;

private:
    static constexpr int kInitialConcurrency = 4;
    static constexpr int kMaxConcurrency = 8;
    static constexpr auto kMinInterval = std::chrono::milliseconds(250);
    static constexpr auto kMaxInterval = std::chrono::milliseconds(8000);
    static constexpr auto kTokenWindow = std::chrono::seconds(60);
    static constexpr auto kMaxPause = std::chrono::seconds(300);
    static constexpr auto kIdleReset = std::chrono::seconds(60);

    struct TokenUse {
        RateClock::time_point when;
        long tokens = 0;
    };

    void prune(RateClock::time_point now);
    void reset_expired_windows(RateClock::time_point now);
    void reset_after_idle(RateClock::time_point now);
    void record_tokens(RateClock::time_point now, long tokens);
    void apply_headers(RateClock::time_point now, const RateHeaders& headers);
    RateClock::time_point token_ready_at(RateClock::time_point now, long tokens) const;
    std::chrono::milliseconds effective_interval(RateClock::time_point now) const;
    int effective_concurrency(RateClock::time_point now) const;
    long effective_token_capacity(RateClock::time_point now) const;

    RateClock::time_point pause_until_{};
    RateClock::time_point last_start_{};
    RateClock::time_point last_activity_{};
    std::chrono::milliseconds interval_{0};
    int concurrency_ = kInitialConcurrency;
    int in_flight_ = 0;
    int consecutive_rate_limits_ = 0;
    int successes_ = 0;
    bool has_activity_ = false;

    std::deque<TokenUse> token_uses_;
    long tokens_in_window_ = 0;
    long token_capacity_ = 0;
    long token_remaining_ = -1;
    RateClock::time_point token_reset_at_{};
    long request_capacity_ = 0;
    long request_remaining_ = -1;
    RateClock::time_point request_reset_at_{};

    std::chrono::milliseconds manual_interval_{0};
    int manual_concurrency_ = 0;
    long manual_token_cap_ = 0;
    RateClock::time_point manual_until_{};

    long learned_tpm_ = 0;
    long learned_rpm_ = 0;
};

// The blocking protection proxy around one provider/account. Requests wait on
// its condition variable; every chat, compressor and subagent that speaks to
// the same provider shares it.
class ProviderGovernor {
public:
    // Admission policy:
    //   Interactive — whenever the budget allows;
    //   Foreground  — when no Interactive request is waiting or in flight;
    //   Background  — when no Interactive or Foreground request is waiting or
    //                 in flight.
    // In-flight requests are never preempted. A Background waiter older than
    // the aging bound is promoted to Foreground for admission, so a worker
    // eventually runs without a user having to do anything.
    bool acquire(long tokens, const CancellationToken& token,
                 RequestClass cls = RequestClass::Interactive);
    void on_response(long status, long retry_after_ms, const RateHeaders& headers, long tokens);
    std::chrono::milliseconds retry_wait() const;
    RateStatus status() const;
    void set_manual(std::chrono::milliseconds interval, int concurrency, long token_cap,
                    std::chrono::milliseconds ttl);
    void clear_manual();

    // How long a Background waiter may wait before it is promoted to Foreground
    // for admission. 60 s shipped; a host may tune it, and tests shorten it.
    void set_background_aging(std::chrono::milliseconds bound);

private:
    static constexpr std::size_t kClassCount = 3;
    static constexpr std::chrono::milliseconds kDefaultBackgroundAging{60000};

    static std::size_t class_index(RequestClass cls) noexcept {
        return static_cast<std::size_t>(cls);
    }
    bool class_admissible(RequestClass cls) const noexcept;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    RateGovernor governor_;
    std::array<int, kClassCount> waiting_{};
    std::array<int, kClassCount> in_flight_{};
    std::chrono::milliseconds background_aging_{kDefaultBackgroundAging};
};

// One governor per provider/account key. A 429 at Anthropic cannot slow an
// OpenAI endpoint; within one account the quota is genuinely shared.
class RateGovernorRegistry {
public:
    static RateGovernorRegistry& global();
    ProviderGovernor& for_config(const Config& cfg);

private:
    std::mutex mutex_;
    std::map<std::string, std::unique_ptr<ProviderGovernor>> governors_;
};

} // namespace agent

#endif // AGENT_RATE_GOVERNOR_H
