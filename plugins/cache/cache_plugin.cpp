#include "cache_plugin.h"

#include "agent/compact.h"
#include "agent/config_paths.h"
#include "agent/llm.h"
#include "agent/private_file.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <utility>

namespace agent::plugins {

namespace {

constexpr int kPriority = 650;
constexpr int kDropPriority = 6;

std::string count_text(long value) {
    return compact::count(value);
}

} // namespace

bool CachePlugin::initialize(const PluginContext& ctx) {
    config_ = ctx.config;
    load_settings();
    subscription_ =
        Events(ctx.event_bus).subscribe<LlmResponseEvent>([this](const LlmResponseEvent& event) {
            on_response(event);
        });
    return true;
}

void CachePlugin::shutdown() {
    subscription_.release();
    std::scoped_lock lk(mtx_);
    windows_.clear();
    config_ = nullptr;
}

void CachePlugin::on_response(const LlmResponseEvent& event) {
    if (event.status != 200)
        return;
    std::scoped_lock lk(mtx_);
    cache::RecordInput input;
    input.at = std::chrono::steady_clock::now();
    input.input = event.prompt_tokens;
    input.read = event.cache_read_tokens;
    input.write = event.cache_write_tokens;
    input.model = event.model;
    input.provider = event.provider;
    input.miss_reason = event.cache_miss_reason;
    windows_[event.owner_id].record(input, static_cast<std::size_t>(samples_.load()));
}

std::chrono::seconds CachePlugin::ttl() const {
    if (!config_)
        return std::chrono::seconds(300);
    if (config_->prompt_cache_ttl == "off")
        return std::chrono::seconds(0);
    if (config_->prompt_cache_ttl == "1h")
        return std::chrono::seconds(3600);
    return std::chrono::seconds(300);
}

std::string CachePlugin::ttl_text() const {
    if (!config_)
        return "5m";
    return config_->prompt_cache_ttl.empty() ? std::string("5m") : config_->prompt_cache_ttl;
}

std::string CachePlugin::settings_path() const {
    return global_config_dir() + "/plugins/cache/cache.conf";
}

void CachePlugin::load_settings() {
    std::ifstream in(settings_path());
    if (!in)
        return;
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "metric") {
            if (value == "calls")
                metric_ = cache::Metric::Calls;
            else if (value == "tokens")
                metric_ = cache::Metric::Tokens;
        } else if (key == "samples" && (value == "4" || value == "8" || value == "16")) {
            samples_.store(std::stoi(value));
        }
    }
}

void CachePlugin::save_settings() const {
    std::string body = "metric=";
    body += metric_ == cache::Metric::Tokens ? "tokens" : "calls";
    body += "\nsamples=";
    body += std::to_string(samples_.load());
    body += "\n";
    (void)write_private_file(settings_path(), body);
}

std::string CachePlugin::percent_text(const cache::Readout& readout) {
    if (readout.sub_one)
        return "<1%";
    return std::to_string(readout.percent) + "%";
}

std::string CachePlugin::age_text(long long seconds) {
    if (seconds < 0)
        seconds = 0;
    if (seconds < 60)
        return std::to_string(seconds) + "s";
    if (seconds < 3600)
        return std::to_string(seconds / 60) + "m";
    return std::to_string(seconds / 3600) + "h";
}

std::string CachePlugin::cold_text(const cache::Readout& readout) const {
    return "cache cold · last request " + age_text(readout.age_seconds) + " ago (TTL " +
           ttl_text() + ")";
}

std::string CachePlugin::warm_text(const cache::Readout& readout) const {
    return "cache warm · wrote " + count_text(readout.write_tokens) + " tokens, no read yet";
}

bool CachePlugin::below_minimum(const cache::Readout& readout) const {
    const int minimum = agent::min_cacheable_tokens(readout.model);
    return readout.read_tokens == 0 && readout.write_tokens == 0 && minimum > 0 &&
           readout.input_tokens < minimum;
}

std::string CachePlugin::reuse_text(const cache::Readout& readout) const {
    std::string line = "cache " + percent_text(readout) + " · ";
    line += (metric_ == cache::Metric::Tokens ? "token reuse" : "call hit");
    line += " · last " + std::to_string(readout.calls) + " calls · window " + ttl_text();
    line += "\n  calls hit: " + std::to_string(readout.hit_calls) + "/" +
            std::to_string(readout.calls) + " (" +
            (readout.call_hit_sub_one ? std::string("<1%")
                                      : std::to_string(readout.call_hit_percent) + "%") +
            ")";
    line += "\n  input " + count_text(readout.input_tokens) + " · read " +
            count_text(readout.read_tokens) + " · wrote " + count_text(readout.write_tokens);
    if (!readout.model.empty() || !readout.provider.empty())
        line +=
            "\n  model " + readout.provider + (readout.provider.empty() ? "" : "/") + readout.model;
    line += "\n  last request " + age_text(readout.age_seconds) + " ago";
    if (!readout.miss_reason.empty())
        line += "\n  last miss: " + readout.miss_reason;
    return line;
}

std::string CachePlugin::state_text(const cache::Readout& readout) const {
    switch (readout.state) {
    case cache::State::NoData:
        return "cache: no completed requests yet";
    case cache::State::Unsupported:
        return "cache: this provider reports no cache counters";
    case cache::State::Cold:
        return cold_text(readout);
    case cache::State::Warm:
        return warm_text(readout);
    case cache::State::Reuse:
        break;
    }
    if (below_minimum(readout))
        return "cache cold · prompt below the " +
               std::to_string(agent::min_cacheable_tokens(readout.model)) + "-token minimum for " +
               readout.model;
    return reuse_text(readout);
}

StatusText CachePlugin::bar(const cache::Readout& readout) const {
    switch (readout.state) {
    case cache::State::NoData:
    case cache::State::Unsupported:
        return {};
    case cache::State::Cold:
        return StatusText{"  cache cold", StatusTone::Dim};
    case cache::State::Warm:
        return StatusText{"  cache warm", StatusTone::Dim};
    case cache::State::Reuse:
        break;
    }
    if (below_minimum(readout))
        return StatusText{"  cache cold", StatusTone::Dim};
    if (readout.percent >= 70)
        return StatusText{"  cache " + percent_text(readout), StatusTone::Good};
    if (readout.percent == 0)
        return StatusText{"  cache " + percent_text(readout), StatusTone::Warn};
    return StatusText{"  cache " + percent_text(readout), StatusTone::Dim};
}

StatusText CachePlugin::render(const StatusSnapshot& snapshot) const {
    if (!config_ || ttl().count() <= 0)
        return {};
    std::scoped_lock lk(mtx_);
    const auto it = windows_.find(snapshot.owner_id);
    if (it == windows_.end())
        return {};
    return bar(it->second.view(std::chrono::steady_clock::now(), metric_,
                               static_cast<std::size_t>(samples_.load()), ttl()));
}

std::string CachePlugin::detail() const {
    if (!config_)
        return "cache: not initialized";
    if (ttl().count() <= 0)
        return "cache: prompt caching is off";
    std::scoped_lock lk(mtx_);
    if (windows_.empty())
        return "cache: no completed requests yet";
    std::ostringstream out;
    for (const auto& [owner, window] : windows_) {
        if (out.tellp() > 0)
            out << "\n\n";
        if (!owner.empty())
            out << "session " << owner << "\n";
        out << state_text(window.view(std::chrono::steady_clock::now(), metric_,
                                      static_cast<std::size_t>(samples_.load()), ttl()));
    }
    return out.str();
}

void CachePlugin::reset() {
    std::scoped_lock lk(mtx_);
    windows_.clear();
}

std::vector<PluginSettingSpec> CachePlugin::settings_specs() {
    PluginSettingSpec metric;
    metric.key = "metric";
    metric.help = "Headline: token reuse (savings) or call hit rate (Hermes-style).";
    metric.type = "choice";
    metric.choices = {"tokens", "calls"};
    metric.value = metric_ == cache::Metric::Tokens ? "tokens" : "calls";
    metric.get = [this] { return metric_ == cache::Metric::Tokens ? "tokens" : "calls"; };
    metric.set = [this](const std::string& value) {
        if (value == "tokens")
            metric_ = cache::Metric::Tokens;
        else if (value == "calls")
            metric_ = cache::Metric::Calls;
        else
            return false;
        save_settings();
        return true;
    };

    PluginSettingSpec samples;
    samples.key = "samples";
    samples.help = "Rolling window size: 4 reacts faster, 16 smooths more.";
    samples.type = "choice";
    samples.choices = {"4", "8", "16"};
    samples.value = std::to_string(samples_.load());
    samples.get = [this] { return std::to_string(samples_.load()); };
    samples.set = [this](const std::string& value) {
        if (value != "4" && value != "8" && value != "16")
            return false;
        samples_.store(std::stoi(value));
        save_settings();
        return true;
    };
    return {std::move(metric), std::move(samples)};
}

std::vector<std::unique_ptr<Capability>> CachePlugin::capabilities() {
    std::vector<std::unique_ptr<Capability>> caps;
    caps.push_back(std::make_unique<StatusSegmentCapability>(
        "cache", kPriority, kDropPriority,
        [this](const StatusSnapshot& snapshot) { return render(snapshot); },
        StatusPlacement{StatusAlign::Left, StatusZone::Bottom}));

    CommandSpec spec;
    spec.name = "cache";
    spec.help = "Recent prompt-cache reuse: a rolling window, not a session average.";
    spec.man = "Usage: /plugin cache — print the recent cache readout for each tracked session. "
               "The headline is token reuse over the last N calls inside the provider cache TTL; "
               "the detail also shows the call hit rate and the read/written token counts. "
               "'/plugin cache reset' clears the window. Settings: '/get plugin settings cache'.";
    spec.subtree = json::parse(R"({
        "reset": {
            "help": "Clear the in-memory cache window.",
            "man": "Clears the samples for every tracked session. The next completed request starts a new window."
        }
    })");
    spec.handlers[""] = [this](const std::string&) { return detail(); };
    spec.handlers["reset"] = [this](const std::string&) {
        reset();
        return std::string("cache: window reset");
    };
    caps.push_back(std::make_unique<CommandCapability>(std::move(spec)));
    caps.push_back(std::make_unique<SettingsCapability>(settings_specs()));
    return caps;
}

} // namespace agent::plugins
