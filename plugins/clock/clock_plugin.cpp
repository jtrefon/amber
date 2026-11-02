#include "clock_plugin.h"

#include "agent/extensions.h"

namespace agent::plugins {

std::string ClockPlugin::format(std::time_t when) {
    std::tm tm{};
    localtime_r(&when, &tm);
    char clk[8];
    std::strftime(clk, sizeof(clk), "%H:%M", &tm);
    return clk;
}

std::vector<std::unique_ptr<Capability>> ClockPlugin::capabilities() {
    std::vector<std::unique_ptr<Capability>> caps;
    // The top rail's right edge: the clock is the last thing the strip gives
    // up, and it costs five columns now that seconds are gone.
    caps.push_back(std::make_unique<StatusSegmentCapability>(
        "clock", /*priority=*/1000, /*drop_priority=*/0,
        [](const StatusSnapshot&) -> StatusText {
            return StatusText{ClockPlugin::format(std::time(nullptr)), StatusTone::Dim};
        },
        StatusPlacement{StatusAlign::Right, StatusZone::Top}));
    return caps;
}

} // namespace agent::plugins
