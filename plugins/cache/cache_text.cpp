#include "cache_text.h"

#include "agent/compact.h"
#include "agent/llm.h"

namespace agent::plugins::cache {

std::string percent_text(const Readout& readout) {
    if (readout.sub_one)
        return "<1%";
    return std::to_string(readout.percent) + "%";
}

std::string age_text(long long seconds) {
    if (seconds < 0)
        seconds = 0;
    if (seconds < 60)
        return std::to_string(seconds) + "s";
    if (seconds < 3600)
        return std::to_string(seconds / 60) + "m";
    return std::to_string(seconds / 3600) + "h";
}

bool below_minimum(const Readout& readout) {
    const int minimum = agent::min_cacheable_tokens(readout.model);
    return readout.read_tokens == 0 && readout.write_tokens == 0 && minimum > 0 &&
           readout.input_tokens < minimum;
}

std::string cold_text(const Readout& readout, const std::string& ttl) {
    return "cache cold · last request " + age_text(readout.age_seconds) + " ago (TTL " + ttl + ")";
}

std::string warm_text(const Readout& readout) {
    return "cache warm · wrote " + agent::compact::count(readout.write_tokens) +
           " tokens, no read yet";
}

std::string reuse_text(const Readout& readout, Metric metric, const std::string& ttl) {
    std::string line = "cache " + percent_text(readout) + " · ";
    line += (metric == Metric::Tokens ? "token reuse" : "call hit");
    line += " · last " + std::to_string(readout.calls) + " calls · window " + ttl;
    line += "\n  calls hit: " + std::to_string(readout.hit_calls) + "/" +
            std::to_string(readout.calls) + " (" +
            (readout.call_hit_sub_one ? std::string("<1%")
                                      : std::to_string(readout.call_hit_percent) + "%") +
            ")";
    line += "\n  input " + agent::compact::count(readout.input_tokens) + " · read " +
            agent::compact::count(readout.read_tokens) + " · wrote " +
            agent::compact::count(readout.write_tokens);
    if (!readout.model.empty() || !readout.provider.empty())
        line +=
            "\n  model " + readout.provider + (readout.provider.empty() ? "" : "/") + readout.model;
    line += "\n  last request " + age_text(readout.age_seconds) + " ago";
    if (!readout.miss_reason.empty())
        line += "\n  last miss: " + readout.miss_reason;
    return line;
}

std::string state_text(const Readout& readout, Metric metric, const std::string& ttl) {
    switch (readout.state) {
    case State::NoData:
        return "cache: no completed requests yet";
    case State::Unsupported:
        return "cache: this provider reports no cache counters";
    case State::Cold:
        return cold_text(readout, ttl);
    case State::Warm:
        return warm_text(readout);
    case State::Reuse:
        break;
    }
    if (below_minimum(readout))
        return "cache cold · prompt below the " +
               std::to_string(agent::min_cacheable_tokens(readout.model)) + "-token minimum for " +
               readout.model;
    return reuse_text(readout, metric, ttl);
}

std::string bar_text(const Readout& readout) {
    switch (readout.state) {
    case State::NoData:
    case State::Unsupported:
        return {};
    case State::Cold:
        return "  cache cold";
    case State::Warm:
        return "  cache warm";
    case State::Reuse:
        break;
    }
    if (below_minimum(readout))
        return "  cache cold";
    return "  cache " + percent_text(readout);
}

} // namespace agent::plugins::cache
