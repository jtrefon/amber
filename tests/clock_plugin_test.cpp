// The clock plugin: the first UI surface to arrive as a contribution rather
// than as a reservation inside the renderer.
//
// Three things must hold: the readout is the same one the renderer used to
// build, it attaches to the right edge, and switching the plugin off takes it
// off the bar (which is the whole point of it being a plugin).

#include "agent/extensions.h"
#include "agent/plugin_runtime.h"
#include "agent/registry.h"
#include "agent/workspace.h"
#include "plugins/clock/clock_plugin.h"
#include "test_util.h"

#include <ctime>
#include <memory>

using namespace agent;
using agent::plugins::ClockPlugin;

namespace {

// A fixed instant, in local time, so the assertion does not depend on the
// machine's zone: the format is what is being pinned.
std::tm local_tm(int hour, int minute, int second) {
    std::tm tm{};
    tm.tm_year = 126; // 2026
    tm.tm_mon = 8;    // September
    tm.tm_mday = 12;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    tm.tm_isdst = -1;
    return tm;
}

std::optional<StatusSegment> find_segment(PluginRuntime& runtime,
                                          const std::string& id) {
    for (const auto& s : runtime.status().render(StatusSnapshot{}))
        if (s.id == id) return s;
    return std::nullopt;
}

} // namespace

TEST(clock_formats_the_time_the_way_the_bar_always_showed_it) {
    std::tm tm = local_tm(9, 5, 7);
    const std::time_t when = std::mktime(&tm);
    ASSERT_EQ(ClockPlugin::format(when), std::string("[09:05:07]"));
}

TEST(clock_formats_midnight_and_noon_without_a_twelve_hour_surprise) {
    std::tm midnight = local_tm(0, 0, 0);
    const std::time_t m = std::mktime(&midnight);
    ASSERT_EQ(ClockPlugin::format(m), std::string("[00:00:00]"));

    std::tm noon = local_tm(12, 0, 0);
    const std::time_t n = std::mktime(&noon);
    ASSERT_EQ(ClockPlugin::format(n), std::string("[12:00:00]"));
}

TEST(clock_attaches_to_the_right_edge) {
    ToolRegistry tools;
    Config cfg;
    Workspace ws;
    PluginRuntime runtime(tools, cfg, ws);
    runtime.add_bundled();
    runtime.start();

    auto segment = find_segment(runtime, "clock");
    ASSERT(segment.has_value());
    ASSERT_TRUE(segment->align == StatusAlign::Right);
    ASSERT(segment->text.find(':') != std::string::npos);
}

TEST(disabling_the_clock_plugin_removes_it_from_the_bar) {
    ToolRegistry tools;
    Config cfg;
    Workspace ws;
    PluginRuntime runtime(tools, cfg, ws);
    runtime.add_bundled();
    runtime.start();
    ASSERT(find_segment(runtime, "clock").has_value());

    ASSERT_TRUE(runtime.apply_state("clock", false));
    ASSERT_FALSE(find_segment(runtime, "clock").has_value());

    // And back, so the ledger's unwind is not a one-way door.
    ASSERT_TRUE(runtime.apply_state("clock", true));
    ASSERT(find_segment(runtime, "clock").has_value());
}

// The bar's other segments are unaffected: the clock is one entry in the
// registry, not a slot the renderer carved out first.
TEST(the_clock_shares_the_registry_with_the_segments_that_were_always_there) {
    ToolRegistry tools;
    Config cfg;
    Workspace ws;
    PluginRuntime runtime(tools, cfg, ws);
    runtime.add_bundled();
    runtime.start();

    bool saw_left = false;
    bool saw_right = false;
    for (const auto& s : runtime.status().render(StatusSnapshot{})) {
        if (s.align == StatusAlign::Left) saw_left = true;
        if (s.align == StatusAlign::Right) saw_right = true;
    }
    ASSERT(saw_left);
    ASSERT(saw_right);
}
