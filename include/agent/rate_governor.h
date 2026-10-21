#ifndef AGENT_RATE_GOVERNOR_H
#define AGENT_RATE_GOVERNOR_H

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "agent/config.h"
#include "agent/process.h"

namespace agent {

using RateClock = std::chrono::steady_clock;

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
    bool acquire(long tokens, const CancellationToken& token);
    void on_response(long status, long retry_after_ms, const RateHeaders& headers, long tokens);
    std::chrono::milliseconds retry_wait() const;
    RateStatus status() const;
    void set_manual(std::chrono::milliseconds interval, int concurrency, long token_cap,
                    std::chrono::milliseconds ttl);
    void clear_manual();

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    RateGovernor governor_;
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
