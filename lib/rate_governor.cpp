#include "agent/rate_governor.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <functional>
#include <random>
#include <sstream>

namespace agent {

namespace {

std::string trimmed(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return {};
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::string lowered(std::string value) {
    for (char& c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

long to_long(const std::string& value) {
    const std::string text = trimmed(value);
    if (text.empty())
        return -1;
    errno = 0;
    char* stop = nullptr;
    const long parsed = std::strtol(text.c_str(), &stop, 10);
    if (errno != 0 || stop == text.c_str() || *stop != '\0' || parsed < 0)
        return -1;
    return parsed;
}

} // namespace

long parse_retry_after_ms(const std::string& value) {
    const std::string text = trimmed(value);
    if (text.empty())
        return 0;
    errno = 0;
    char* stop = nullptr;
    const long seconds = std::strtol(text.c_str(), &stop, 10);
    if (errno != 0 || stop == text.c_str() || *stop != '\0' || seconds < 0)
        return 0; // an HTTP-date or garbage: the policy's exponential fallback
    return std::min(seconds * 1000L, 300000L);
}

namespace {

double duration_unit_factor(const std::string& unit) {
    if (unit == "ms")
        return 1;
    if (unit == "s")
        return 1000;
    if (unit == "m")
        return 60000;
    if (unit == "h")
        return 3600000;
    return 0;
}

// One "<number><unit>" component. Returns false on anything malformed.
bool parse_duration_component(const std::string& text, std::size_t& i, long& out_ms) {
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])))
        ++i;
    const std::size_t number_start = i;
    while (i < text.size() && (std::isdigit(static_cast<unsigned char>(text[i])) || text[i] == '.'))
        ++i;
    if (i == number_start)
        return false;
    const std::string number = text.substr(number_start, i - number_start);
    const std::size_t unit_start = i;
    while (i < text.size() && std::isalpha(static_cast<unsigned char>(text[i])))
        ++i;
    const double factor = duration_unit_factor(lowered(text.substr(unit_start, i - unit_start)));
    if (factor == 0)
        return false;
    try {
        out_ms = static_cast<long>(std::stod(number) * factor);
    } catch (...) {
        return false;
    }
    return true;
}

} // namespace

long parse_rate_duration_ms(const std::string& value) {
    const std::string text = trimmed(value);
    if (text.empty())
        return 0;
    // A bare integer is seconds, matching Retry-After.
    const bool bare =
        std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    if (bare) {
        const long seconds = to_long(text);
        return seconds < 0 ? 0 : std::min(seconds * 1000L, 300000L);
    }

    long total = 0;
    std::size_t i = 0;
    bool any = false;
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])))
            ++i;
        if (i >= text.size())
            break;
        long part = 0;
        if (!parse_duration_component(text, i, part))
            return 0;
        total += part;
        any = true;
    }
    return any ? std::min(total, 3600000L) : 0;
}

void capture_rate_header(const std::string& name, const std::string& value, RateHeaders& out) {
    const std::string key = lowered(trimmed(name));
    if (key == "x-ratelimit-limit-requests" || key == "anthropic-ratelimit-requests-limit") {
        out.request_limit = to_long(value);
    } else if (key == "x-ratelimit-remaining-requests" ||
               key == "anthropic-ratelimit-requests-remaining") {
        out.request_remaining = to_long(value);
    } else if (key == "x-ratelimit-reset-requests" || key == "anthropic-ratelimit-requests-reset") {
        out.request_reset_ms = parse_rate_duration_ms(value);
    } else if (key == "x-ratelimit-limit-tokens" || key == "anthropic-ratelimit-tokens-limit") {
        out.token_limit = to_long(value);
    } else if (key == "x-ratelimit-remaining-tokens" ||
               key == "anthropic-ratelimit-tokens-remaining") {
        out.token_remaining = to_long(value);
    } else if (key == "x-ratelimit-reset-tokens" || key == "anthropic-ratelimit-tokens-reset") {
        out.token_reset_ms = parse_rate_duration_ms(value);
    }
}

RateGovernor::RateGovernor() = default;

void RateGovernor::prune(RateClock::time_point now) {
    while (!token_uses_.empty() && token_uses_.front().when + kTokenWindow <= now) {
        tokens_in_window_ = std::max(0L, tokens_in_window_ - token_uses_.front().tokens);
        token_uses_.pop_front();
    }
}

void RateGovernor::record_tokens(RateClock::time_point now, long tokens) {
    if (tokens <= 0)
        return;
    token_uses_.push_back({now, tokens});
    tokens_in_window_ += tokens;
    prune(now);
}

void RateGovernor::apply_headers(RateClock::time_point now, const RateHeaders& headers) {
    if (headers.request_limit > 0)
        request_capacity_ = headers.request_limit;
    if (headers.request_remaining >= 0)
        request_remaining_ = headers.request_remaining;
    if (headers.request_reset_ms > 0)
        request_reset_at_ = now + std::chrono::milliseconds(headers.request_reset_ms);
    if (headers.token_limit > 0)
        token_capacity_ = headers.token_limit;
    if (headers.token_remaining >= 0) {
        token_remaining_ = headers.token_remaining;
        if (token_capacity_ > 0)
            tokens_in_window_ = std::max(0L, token_capacity_ - token_remaining_);
    }
    if (headers.token_reset_ms > 0)
        token_reset_at_ = now + std::chrono::milliseconds(headers.token_reset_ms);
    if (request_remaining_ == 0 && request_reset_at_ > now)
        pause_until_ = std::max(pause_until_, request_reset_at_);
    if (token_remaining_ == 0 && token_reset_at_ > now)
        pause_until_ = std::max(pause_until_, token_reset_at_);
}

std::chrono::milliseconds RateGovernor::effective_interval(RateClock::time_point now) const {
    if (manual_until_ > now && manual_interval_.count() > 0)
        return std::max(interval_, manual_interval_);
    return interval_;
}

int RateGovernor::effective_concurrency(RateClock::time_point now) const {
    if (manual_until_ > now && manual_concurrency_ > 0)
        return std::min(concurrency_, manual_concurrency_);
    return concurrency_;
}

long RateGovernor::effective_token_capacity(RateClock::time_point now) const {
    long cap = token_capacity_;
    if (manual_until_ > now && manual_token_cap_ > 0)
        cap = cap > 0 ? std::min(cap, manual_token_cap_) : manual_token_cap_;
    return cap;
}

void RateGovernor::reset_expired_windows(RateClock::time_point now) {
    if (request_reset_at_ != RateClock::time_point{} && request_reset_at_ <= now &&
        request_capacity_ > 0) {
        request_remaining_ = request_capacity_;
        request_reset_at_ = RateClock::time_point{};
    }
    if (token_reset_at_ != RateClock::time_point{} && token_reset_at_ <= now &&
        token_capacity_ > 0) {
        token_remaining_ = token_capacity_;
        tokens_in_window_ = 0;
        token_uses_.clear();
        token_reset_at_ = RateClock::time_point{};
    }
}

void RateGovernor::reset_after_idle(RateClock::time_point now) {
    // The pause itself (a server deadline) is never discarded, only the
    // learned spacing: idle time is evidence the provider recovered.
    if (!has_activity_ || now - last_activity_ <= kIdleReset)
        return;
    interval_ = std::chrono::milliseconds(0);
    concurrency_ = kInitialConcurrency;
    consecutive_rate_limits_ = 0;
    successes_ = 0;
}

RateClock::time_point RateGovernor::token_ready_at(RateClock::time_point now, long tokens) const {
    const long cap = effective_token_capacity(now);
    if (cap <= 0 || tokens <= 0)
        return now;
    long available = token_remaining_ >= 0 ? token_remaining_ : cap - tokens_in_window_;
    if (available >= tokens)
        return now;
    if (token_reset_at_ > now)
        return token_reset_at_;
    if (token_uses_.empty())
        return now + std::chrono::seconds(1);
    long over = tokens - available;
    for (const auto& use : token_uses_) {
        over -= use.tokens;
        if (over <= 0)
            return use.when + kTokenWindow;
    }
    return token_uses_.back().when + kTokenWindow;
}

std::chrono::milliseconds RateGovernor::wait_before(RateClock::time_point now, long tokens) {
    prune(now);
    reset_expired_windows(now);
    reset_after_idle(now);
    auto until = std::max(pause_until_, last_start_ + effective_interval(now));
    if (in_flight_ >= effective_concurrency(now))
        until = std::max(until, now + std::chrono::milliseconds(1));
    until = std::max(until, token_ready_at(now, tokens));
    return until > now ? std::chrono::duration_cast<std::chrono::milliseconds>(until - now)
                       : std::chrono::milliseconds(0);
}

void RateGovernor::on_start(RateClock::time_point now) {
    ++in_flight_;
    last_start_ = now;
    last_activity_ = now;
    has_activity_ = true;
}

void RateGovernor::on_success(RateClock::time_point now, long tokens, const RateHeaders& headers) {
    if (in_flight_ > 0)
        --in_flight_;
    last_activity_ = now;
    has_activity_ = true;
    apply_headers(now, headers);
    consecutive_rate_limits_ = 0;
    ++successes_;
    // Fast recovery: a success is headroom. Half the spacing per success and
    // clear it after two; the next 429 starts a new AIMD cycle.
    if (interval_.count() > 0) {
        interval_ = interval_ / 2;
        if (interval_ < kMinInterval || successes_ >= 2)
            interval_ = std::chrono::milliseconds(0);
    }
    concurrency_ = std::min(kMaxConcurrency, concurrency_ + 1);
    if (token_remaining_ < 0)
        record_tokens(now, tokens);
    learned_tpm_ = tokens_in_window_ > 0 ? tokens_in_window_ * 60 : learned_tpm_;
    learned_rpm_ = learned_rpm_ > 0 ? learned_rpm_ : 0;
}

void RateGovernor::on_rate_limit(RateClock::time_point now, std::chrono::milliseconds retry_after,
                                 const RateHeaders& headers, std::chrono::milliseconds jitter,
                                 long tokens) {
    if (in_flight_ > 0)
        --in_flight_;
    last_activity_ = now;
    has_activity_ = true;
    apply_headers(now, headers);
    ++consecutive_rate_limits_;
    successes_ = 0;

    auto pause = retry_after;
    if (pause.count() <= 0) {
        const int shift = std::min(consecutive_rate_limits_ - 1, 6);
        pause = std::chrono::milliseconds(1000LL * (1 << shift));
    }
    pause =
        std::min(pause + jitter, std::chrono::duration_cast<std::chrono::milliseconds>(kMaxPause));
    pause_until_ = std::max(pause_until_, now + pause);

    // The first rejection is a one-off pause; the provider's own Retry-After
    // is the answer. Only sustained rejection is congestion.
    if (consecutive_rate_limits_ >= 2) {
        interval_ = std::min(kMaxInterval, interval_.count() == 0 ? kMinInterval : interval_ * 2);
        concurrency_ = std::max(1, concurrency_ / 2);
    }
    if (token_remaining_ < 0)
        record_tokens(now, tokens);
}

void RateGovernor::on_failure(RateClock::time_point now) {
    if (in_flight_ > 0)
        --in_flight_;
    last_activity_ = now;
    has_activity_ = true;
}

void RateGovernor::set_manual(RateClock::time_point now, std::chrono::milliseconds interval,
                              int concurrency, long token_cap, std::chrono::milliseconds ttl) {
    manual_interval_ = std::max(std::chrono::milliseconds(0), interval);
    manual_concurrency_ = concurrency > 0 ? concurrency : 0;
    manual_token_cap_ = token_cap > 0 ? token_cap : 0;
    manual_until_ = now + std::max(std::chrono::milliseconds(1), ttl);
}

void RateGovernor::clear_manual() {
    manual_interval_ = std::chrono::milliseconds(0);
    manual_concurrency_ = 0;
    manual_token_cap_ = 0;
    manual_until_ = RateClock::time_point{};
}

std::chrono::milliseconds RateGovernor::retry_wait(RateClock::time_point now) const {
    auto until = std::max(pause_until_, last_start_ + effective_interval(now));
    return until > now ? std::chrono::duration_cast<std::chrono::milliseconds>(until - now)
                       : std::chrono::milliseconds(0);
}

RateStatus RateGovernor::status(RateClock::time_point now) const {
    RateStatus out;
    out.interval_ms = static_cast<long>(effective_interval(now).count());
    const auto pause =
        pause_until_ > now
            ? std::chrono::duration_cast<std::chrono::milliseconds>(pause_until_ - now)
            : std::chrono::milliseconds(0);
    out.pause_ms = static_cast<long>(pause.count());
    out.concurrency = effective_concurrency(now);
    out.in_flight = in_flight_;
    out.consecutive_rate_limits = consecutive_rate_limits_;
    out.successes = successes_;
    out.tokens_in_window = tokens_in_window_;
    out.token_capacity = effective_token_capacity(now);
    out.request_capacity = request_capacity_;
    out.learned_tpm = learned_tpm_;
    out.learned_rpm = learned_rpm_;
    out.manual = manual_until_ > now;
    out.manual_remaining_ms =
        out.manual ? static_cast<long>(
                         std::chrono::duration_cast<std::chrono::milliseconds>(manual_until_ - now)
                             .count())
                   : 0;
    return out;
}

bool ProviderGovernor::acquire(long tokens, const CancellationToken& token) {
    std::unique_lock lock(mutex_);
    for (;;) {
        if (token.is_requested())
            return false;
        const auto now = RateClock::now();
        const auto wait = governor_.wait_before(now, tokens);
        if (wait.count() == 0) {
            governor_.on_start(now);
            return true;
        }
        cv_.wait_for(lock, std::min(wait, std::chrono::milliseconds(200)));
    }
}

void ProviderGovernor::on_response(long status, long retry_after_ms, const RateHeaders& headers,
                                   long tokens) {
    std::scoped_lock lock(mutex_);
    const auto now = RateClock::now();
    if (status == 429 || status == 529) {
        auto jitter = std::chrono::milliseconds(0);
        if (retry_after_ms <= 0) {
            thread_local std::mt19937 rng(std::random_device{}());
            std::uniform_int_distribution<int> spread(0, 250);
            jitter = std::chrono::milliseconds(spread(rng));
        }
        governor_.on_rate_limit(now, std::chrono::milliseconds(retry_after_ms), headers, jitter,
                                tokens);
    } else if (status >= 200 && status < 300) {
        governor_.on_success(now, tokens, headers);
    } else {
        governor_.on_failure(now);
    }
    cv_.notify_all();
}

std::chrono::milliseconds ProviderGovernor::retry_wait() const {
    std::scoped_lock lock(mutex_);
    return governor_.retry_wait(RateClock::now());
}

RateStatus ProviderGovernor::status() const {
    std::scoped_lock lock(mutex_);
    return governor_.status(RateClock::now());
}

void ProviderGovernor::set_manual(std::chrono::milliseconds interval, int concurrency,
                                  long token_cap, std::chrono::milliseconds ttl) {
    std::scoped_lock lock(mutex_);
    governor_.set_manual(RateClock::now(), interval, concurrency, token_cap, ttl);
    cv_.notify_all();
}

void ProviderGovernor::clear_manual() {
    std::scoped_lock lock(mutex_);
    governor_.clear_manual();
    cv_.notify_all();
}

RateGovernorRegistry& RateGovernorRegistry::global() {
    static RateGovernorRegistry registry;
    return registry;
}

ProviderGovernor& RateGovernorRegistry::for_config(const Config& cfg) {
    const std::size_t key_hash = std::hash<std::string>{}(cfg.api_key);
    const std::string key =
        cfg.provider_name + "\x1f" + cfg.api_base + "\x1f" + std::to_string(key_hash);
    std::scoped_lock lock(mutex_);
    auto it = governors_.find(key);
    if (it == governors_.end())
        it = governors_.emplace(key, std::make_unique<ProviderGovernor>()).first;
    return *it->second;
}

} // namespace agent
