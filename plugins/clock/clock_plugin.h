#ifndef AGENT_PLUGINS_CLOCK_H
#define AGENT_PLUGINS_CLOCK_H

#include "agent/plugin_core.h"

#include <ctime>
#include <string>

namespace agent::plugins {

// The time, at the right end of the status bar.
//
// This was core UI: draw_status_bar built the string itself and reserved its
// width before it knew what else the bar held. A clock is not the harness's
// business - it is a readout that attaches to an edge and renders - so it is a
// plugin, and the bar's right edge is a registered region rather than a
// hardcoded reservation. Switch it off with `/set plugin clock off`.
//
// It reads the clock at paint time and holds no state: the host repaints the
// bar on its own second, so there is nothing to schedule and nothing to cache.
class ClockPlugin : public IPlugin {
public:
    std::string id() const override { return "clock"; }
    std::string version() const override { return "1.0.0"; }
    std::string name() const override { return "Clock"; }
    std::string description() const override {
        return "Shows the time at the right end of the status bar.";
    }
    std::string category() const override { return plugin_category::kUi; }

    bool initialize(const PluginContext&) override { return true; }
    void shutdown() override {}

    std::vector<std::unique_ptr<Capability>> capabilities() override;

    // The readout on its own, so the format is pinned by a test rather than by
    // looking at a terminal.
    static std::string format(std::time_t when);
};

} // namespace agent::plugins

#endif // AGENT_PLUGINS_CLOCK_H
