#include "cache_plugin.h"

#include "cache_text.h"

#include "agent/config_paths.h"
#include "agent/private_file.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace agent::plugins {

namespace {

constexpr int kPriority = 650;
constexpr int kDropPriority = 6;

// The settings file is a fixed child of the canonical config root: a path that
// somehow escapes that root is ignored rather than opened. This is the same
// barrier every other plugin-owned settings file uses.
std::string resolve_settings_path() {
    const std::filesystem::path root =
        std::filesystem::weakly_canonical(agent::global_config_dir());
    const std::filesystem::path file =
        std::filesystem::weakly_canonical(root / "plugins" / "cache" / "cache.conf");
    if (file.generic_string().rfind(root.generic_string() + "/", 0) != 0)
        return {};
    return file.string();
}

} // namespace

bool CachePlugin::initialize(const PluginContext& ctx) {
    config_ = ctx.config;
    settings_path_ = resolve_settings_path();
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
    settings_path_.clear();
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
    if (config_->prompt_cache_ttl == "off")
        return std::chrono::seconds(0);
    if (config_->prompt_cache_ttl == "1h")
        return std::chrono::seconds(3600);
    return std::chrono::seconds(300);
}

std::string CachePlugin::ttl_text() const {
    return config_->prompt_cache_ttl.empty() ? std::string("5m") : config_->prompt_cache_ttl;
}

void CachePlugin::load_settings() {
    std::ifstream in(settings_path_);
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
    if (settings_path_.empty())
        return;
    std::string body = "metric=";
    body += metric_ == cache::Metric::Tokens ? "tokens" : "calls";
    body += "\nsamples=";
    body += std::to_string(samples_.load());
    body += "\n";
    (void)write_private_file(settings_path_, body);
}

StatusText CachePlugin::render(const StatusSnapshot& snapshot) const {
    if (!config_ || ttl().count() <= 0)
        return {};
    std::scoped_lock lk(mtx_);
    const auto it = windows_.find(snapshot.owner_id);
    if (it == windows_.end())
        return {};
    const cache::Readout readout =
        it->second.view(std::chrono::steady_clock::now(), metric_,
                        static_cast<std::size_t>(samples_.load()), ttl());
    const std::string text = cache::bar_text(readout);
    if (text.empty())
        return {};
    StatusTone tone = StatusTone::Dim;
    if (readout.state == cache::State::Reuse && !cache::below_minimum(readout)) {
        if (readout.percent >= 70)
            tone = StatusTone::Good;
        else if (readout.percent == 0)
            tone = StatusTone::Warn;
    }
    return StatusText{text, tone};
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
        out << cache::state_text(window.view(std::chrono::steady_clock::now(), metric_,
                                             static_cast<std::size_t>(samples_.load()), ttl()),
                                 metric_, ttl_text());
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
