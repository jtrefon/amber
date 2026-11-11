#include "cache_reuse.h"

#include <algorithm>

namespace agent::plugins::cache {

namespace {

std::size_t bounded(std::size_t samples) {
    return std::max<std::size_t>(1, samples);
}

// Positive ratios below 1: truncation is deliberate. A gauge that rounds
// 99.7 up to 100 claims a fully cached window that does not exist.
int floor_percent(double ratio) {
    if (ratio <= 0.0)
        return 0;
    if (ratio >= 1.0)
        return 100;
    return static_cast<int>(ratio * 100.0);
}

} // namespace

CacheWindow::Record CacheWindow::record(const RecordInput& input, std::size_t max_samples) {
    const bool vendor_changed =
        (!input.model.empty() && !model_.empty() && input.model != model_) ||
        (!input.provider.empty() && !provider_.empty() && input.provider != provider_);
    if (vendor_changed) {
        samples_.clear();
        miss_reason_.clear();
        unsupported_ = false;
    }
    if (!input.model.empty())
        model_ = input.model;
    if (!input.provider.empty())
        provider_ = input.provider;

    if (input.input <= 0)
        return Record::Ignored;
    if (input.read < 0) {
        // A provider that reports no counters contributes no gauge, not a
        // fabricated zero. A later supported exchange clears this state.
        unsupported_ = true;
        samples_.clear();
        miss_reason_.clear();
        return Record::Unsupported;
    }

    unsupported_ = false;
    Sample sample;
    sample.at = input.at;
    sample.input = input.input;
    sample.read = std::min(input.read, input.input);
    sample.write = std::min(std::max(0L, input.write), input.input);
    samples_.push_back(sample);
    while (samples_.size() > bounded(max_samples))
        samples_.pop_front();
    if (!input.miss_reason.empty())
        miss_reason_ = input.miss_reason;
    return Record::Accepted;
}

void CacheWindow::reset() {
    samples_.clear();
    model_.clear();
    provider_.clear();
    miss_reason_.clear();
    unsupported_ = false;
}

std::vector<const Sample*> CacheWindow::recent_samples(std::chrono::steady_clock::time_point now,
                                                       std::size_t max_samples,
                                                       std::chrono::seconds ttl) const {
    std::vector<const Sample*> recent;
    const std::size_t cap = bounded(max_samples);
    for (auto it = samples_.rbegin(); it != samples_.rend() && recent.size() < cap; ++it) {
        if (now > it->at && now - it->at > ttl)
            break; // older samples are behind the newest in the deque
        recent.push_back(&*it);
    }
    std::reverse(recent.begin(), recent.end());
    return recent;
}

Readout CacheWindow::summarize(const std::vector<const Sample*>& recent, Metric metric) {
    Readout out;
    for (const Sample* sample : recent) {
        out.input_tokens += sample->input;
        out.read_tokens += sample->read;
        out.write_tokens += sample->write;
        ++out.calls;
        if (sample->read > 0)
            ++out.hit_calls;
    }
    if (out.input_tokens <= 0 || out.calls <= 0) {
        out.state = State::NoData;
        return out;
    }

    const double ratio = metric == Metric::Calls
                             ? static_cast<double>(out.hit_calls) / out.calls
                             : static_cast<double>(out.read_tokens) / out.input_tokens;
    out.percent = floor_percent(ratio);
    out.sub_one = ratio > 0.0 && ratio * 100.0 < 1.0;
    out.call_hit_percent = floor_percent(static_cast<double>(out.hit_calls) / out.calls);
    out.call_hit_sub_one = out.hit_calls > 0 && out.hit_calls * 100 < out.calls;
    out.state =
        out.hit_calls > 0 ? State::Reuse : (out.write_tokens > 0 ? State::Warm : State::Reuse);
    return out;
}

Readout CacheWindow::view(std::chrono::steady_clock::time_point now, Metric metric,
                          std::size_t max_samples, std::chrono::seconds ttl) const {
    Readout out;
    out.model = model_;
    out.provider = provider_;
    if (unsupported_) {
        out.state = State::Unsupported;
        return out;
    }
    if (samples_.empty()) {
        out.state = State::NoData;
        return out;
    }

    const auto newest = samples_.back().at;
    const auto newest_age =
        now > newest ? now - newest : std::chrono::steady_clock::duration::zero();
    out.age_seconds = std::chrono::duration_cast<std::chrono::seconds>(newest_age).count();
    if (ttl.count() <= 0 || newest_age > ttl) {
        out.state = State::Cold;
        return out;
    }

    const auto recent = recent_samples(now, max_samples, ttl);
    if (recent.empty()) {
        out.state = State::Cold;
        return out;
    }

    Readout summary = summarize(recent, metric);
    summary.model = model_;
    summary.provider = provider_;
    summary.age_seconds = out.age_seconds;
    summary.miss_reason = miss_reason_;
    return summary;
}

} // namespace agent::plugins::cache
