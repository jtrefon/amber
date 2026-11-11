#ifndef AMBER_PLUGINS_CACHE_CACHE_PLUGIN_H
#define AMBER_PLUGINS_CACHE_CACHE_PLUGIN_H

#include "cache_reuse.h"

#include "agent/events.h"
#include "agent/extensions.h"
#include "agent/plugin_core.h"

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace agent::plugins {

// A bundled observability plugin: the recent prompt-cache reuse gauge.
//
// It contributes a bottom-bar status segment, a `/plugin cache` readout and
// two settings. The model is deliberately pure (cache_reuse.h); the plugin is
// only the event adapter, the per-session store and the text surfaces.
class CachePlugin final : public IPlugin {
public:
    std::string id() const override { return "cache"; }
    std::string version() const override { return "1.0.0"; }
    std::string name() const override { return "Cache"; }
    std::string description() const override {
        return "Recent prompt-cache reuse: a rolling window, not a session average.";
    }
    std::string category() const override { return plugin_category::kObservability; }

    bool initialize(const PluginContext& ctx) override;
    void shutdown() override;
    std::vector<std::unique_ptr<Capability>> capabilities() override;

private:
    StatusText render(const StatusSnapshot& snapshot) const;
    std::string detail() const;
    void reset();
    void on_response(const LlmResponseEvent& event);
    std::chrono::seconds ttl() const;
    std::string ttl_text() const;
    void load_settings();
    void save_settings() const;
    std::vector<PluginSettingSpec> settings_specs();

    const Config* config_ = nullptr;
    Subscription subscription_;
    mutable std::mutex mtx_;
    std::map<std::string, cache::CacheWindow> windows_;
    std::string settings_path_;
    cache::Metric metric_ = cache::Metric::Tokens;
    std::atomic<int> samples_{8};
};

} // namespace agent::plugins

#endif // AMBER_PLUGINS_CACHE_CACHE_PLUGIN_H
