#ifndef AGENT_RATE_LIMIT_H
#define AGENT_RATE_LIMIT_H

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>

#include "agent/process.h"

namespace agent {

// Adaptive state shared by every LLM request. A 429 is not a transient network
// blip: the provider is telling us to slow down. Each one doubles the spacing
// (cap 8 s) and sets a cooldown (Retry-After when given, else exponential
// 1/2/4/.../60 s). Four successes decay the spacing back toward 250 ms.
struct RateLimitPolicy {
    std::chrono::milliseconds interval{250};
    std::chrono::milliseconds cooldown{0};
    int strikes = 0;
    int successes = 0;

    void note_rate_limit(std::chrono::milliseconds retry_after) {
        ++strikes;
        successes = 0;
        const int step = std::min(strikes, 6);
        const auto exponential = std::chrono::milliseconds(1000LL * (1 << (step - 1)));
        cooldown = retry_after.count() > 0
                       ? std::min(retry_after, std::chrono::milliseconds(300000))
                       : std::min(exponential, std::chrono::milliseconds(60000));
        interval = std::min(std::chrono::milliseconds(8000),
                            std::max(std::chrono::milliseconds(500), interval * 2));
    }

    void note_success() {
        ++successes;
        strikes = 0;
        cooldown = std::chrono::milliseconds(0);
        if (successes >= 4) {
            successes = 0;
            interval = interval > std::chrono::milliseconds(250)
                           ? std::max(std::chrono::milliseconds(250),
                                      interval - std::chrono::milliseconds(250))
                           : std::chrono::milliseconds(250);
        }
    }
};

// Numeric Retry-After seconds -> ms; an HTTP-date or garbage yields 0 (the
// policy's exponential fallback is then used).
long parse_retry_after_ms(const std::string& value);

// Process-wide gate: every LLM HTTP request goes through one pacer, so chat,
// compression and every subagent share the same spacing and cooldown. A
// request waits at most 200 ms at a time, so cancellation is prompt.
class RequestPacer {
public:
    static RequestPacer& global();

    // Returns false when the token fires while waiting.
    bool before_request(const CancellationToken& token);
    void on_rate_limit(std::chrono::milliseconds retry_after);
    void on_success();
    RateLimitPolicy policy() const;

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    RateLimitPolicy policy_;
    std::chrono::steady_clock::time_point cooldown_until_{};
    std::chrono::steady_clock::time_point last_start_{};
};

} // namespace agent

#endif // AGENT_RATE_LIMIT_H
