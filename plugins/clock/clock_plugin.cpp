#include "clock_plugin.h"

#include "agent/extensions.h"

namespace agent::plugins {

std::string ClockPlugin::format(std::time_t when) {
    std::tm tm{};
    localtime_r(&when, &tm);
    char clk[16];
    std::strftime(clk, sizeof(clk), "[%H:%M:%S]", &tm);
    return clk;
}

std::vector<std::unique_ptr<Capability>> ClockPlugin::capabilities() {
    std::vector<std::unique_ptr<Capability>> caps;
    // The right zone orders ascending, so the highest priority sits closest to
    // the edge: the clock stays where it has always been. Drop priority zero -
    // it is the last thing the bar should give up, and it costs nine columns.
    caps.push_back(std::make_unique<StatusSegmentCapability>(
        "clock", /*priority=*/1000, /*drop_priority=*/0,
        [](const StatusSnapshot&) -> StatusText {
            return StatusText{ClockPlugin::format(std::time(nullptr)), StatusTone::Dim};
        },
        StatusAlign::Right));
    return caps;
}

} // namespace agent::plugins
