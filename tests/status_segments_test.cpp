// Status segment registry and amber's own segments. Hermetic: the core
// segments are pure functions of a snapshot, so the bar is testable without
// ncurses or a terminal.

#include "agent/core_segments.h"
#include "agent/extensions.h"
#include "test_util.h"

#include <string>
#include <vector>

using namespace agent;

namespace {

StatusSnapshot base_snapshot() {
    StatusSnapshot s;
    s.window_index = 2;
    s.window_count = 3;
    s.model = "qwen3";
    s.reasoning_effort = "high";
    s.mode = AgentMode::Write;
    s.latency_ms = 240;
    s.tps = 47.4;
    s.prompt_tokens = 12500;
    s.completion_tokens = 340;
    return s;
}

// Register the core segments once per test into a fresh registry.
StatusRegistry core_registry() {
    StatusRegistry registry;
    register_core_status_segments(registry);
    return registry;
}

std::string text_of(const std::vector<StatusSegment>& segments,
                    const std::string& id) {
    for (const auto& s : segments)
        if (s.id == id) return s.text;
    return {};
}

} // namespace

TEST(status_registry_orders_by_priority_then_registration) {
    StatusRegistry registry;
    registry.add("plug", "late", 300, 0, [](const StatusSnapshot&) {
        return StatusText{"C", StatusTone::Dim};
    });
    registry.add("plug", "early", 100, 0, [](const StatusSnapshot&) {
        return StatusText{"A", StatusTone::Dim};
    });
    registry.add("plug", "tie", 300, 0, [](const StatusSnapshot&) {
        return StatusText{"D", StatusTone::Dim};
    });

    auto out = registry.render(StatusSnapshot{});
    ASSERT_EQ(out.size(), 3u);
    ASSERT_EQ(out[0].text, std::string("A"));
    ASSERT_EQ(out[1].text, std::string("C"));
    ASSERT_EQ(out[2].text, std::string("D"));  // same priority: registration order
}

TEST(status_registry_skips_segments_that_decline) {
    StatusRegistry registry;
    registry.add("plug", "hidden", 100, 0, [](const StatusSnapshot&) {
        return StatusText{};  // empty text: not shown
    });
    registry.add("plug", "shown", 200, 0, [](const StatusSnapshot&) {
        return StatusText{"here", StatusTone::Good};
    });

    auto out = registry.render(StatusSnapshot{});
    ASSERT_EQ(out.size(), 1u);
    ASSERT_EQ(out[0].id, std::string("shown"));
    ASSERT_TRUE(out[0].tone == StatusTone::Good);
}

TEST(status_registry_carries_drop_priority) {
    StatusRegistry registry;
    registry.add("plug", "keep", 100, 0, [](const StatusSnapshot&) {
        return StatusText{"K", StatusTone::Dim};
    });
    registry.add("plug", "droppable", 200, 9, [](const StatusSnapshot&) {
        return StatusText{"D", StatusTone::Dim};
    });

    auto out = registry.render(StatusSnapshot{});
    ASSERT_EQ(out.size(), 2u);
    ASSERT_EQ(out[0].drop_priority, 0);
    ASSERT_EQ(out[1].drop_priority, 9);
}

TEST(status_registry_removal_takes_only_that_segment) {
    StatusRegistry registry;
    auto keep = registry.add("plug", "keep", 100, 0, [](const StatusSnapshot&) {
        return StatusText{"K", StatusTone::Dim};
    });
    auto drop = registry.add("plug", "drop", 200, 0, [](const StatusSnapshot&) {
        return StatusText{"D", StatusTone::Dim};
    });
    ASSERT_EQ(registry.size(), 2u);

    drop.remove();
    ASSERT_EQ(registry.render(StatusSnapshot{}).size(), 1u);
    keep.remove();
    ASSERT_EQ(registry.size(), 0u);
    ASSERT_TRUE(registry.render(StatusSnapshot{}).empty());
}

TEST(status_registry_reports_owners) {
    StatusRegistry registry;
    registry.add("gemini", "balance", 800, 5, [](const StatusSnapshot&) {
        return StatusText{"$1.00", StatusTone::Dim};
    });
    auto items = registry.items();
    ASSERT_EQ(items.size(), 1u);
    ASSERT_EQ(items[0].owner, std::string("gemini"));
    ASSERT_EQ(items[0].name, std::string("balance"));
    ASSERT_TRUE(items[0].kind == CapabilityKind::StatusSegment);
}

TEST(core_segments_render_the_expected_bar) {
    auto registry = core_registry();
    auto out = registry.render(base_snapshot());

    ASSERT_EQ(text_of(out, "window"), std::string("[2/3]"));
    ASSERT_EQ(text_of(out, "model"), std::string(" [qwen3(high)]"));
    ASSERT_EQ(text_of(out, "mode"), std::string(" write "));
    ASSERT_EQ(text_of(out, "lag"), std::string("  lag 240ms"));
    ASSERT_EQ(text_of(out, "tps"), std::string("  47 t/s"));
    // 12500 -> "12k"; 340 stays literal.
    const std::string tokens = text_of(out, "tokens");
    ASSERT(tokens.find("12k") != std::string::npos);
    ASSERT(tokens.find("340") != std::string::npos);
}

TEST(core_segments_mode_words_and_tones) {
    auto registry = core_registry();

    StatusSnapshot read = base_snapshot();
    read.mode = AgentMode::Read;
    auto read_out = registry.render(read);
    ASSERT_EQ(text_of(read_out, "mode"), std::string(" read "));
    for (const auto& s : read_out)
        if (s.id == "mode") ASSERT_TRUE(s.tone == StatusTone::Good);

    StatusSnapshot yolo = base_snapshot();
    yolo.mode = AgentMode::Yolo;
    auto yolo_out = registry.render(yolo);
    ASSERT_EQ(text_of(yolo_out, "mode"), std::string(" yolo "));
    for (const auto& s : yolo_out)
        if (s.id == "mode") ASSERT_TRUE(s.tone == StatusTone::Accent);
}

TEST(core_segments_omit_what_is_not_known) {
    auto registry = core_registry();
    StatusSnapshot unknown;
    unknown.window_count = 1;
    unknown.window_index = 1;
    unknown.model = "m";
    unknown.mode = AgentMode::Read;
    // No latency, no tps, no token counts, no jobs, no tool, no balance, no mcp.

    auto out = registry.render(unknown);
    // The placeholders are still shown (the bar has always reserved the room)...
    ASSERT_FALSE(text_of(out, "lag").empty());
    ASSERT_FALSE(text_of(out, "tps").empty());
    // ...while the segments with nothing to say are absent entirely.
    ASSERT_TRUE(text_of(out, "scroll").empty());
    ASSERT_TRUE(text_of(out, "balance").empty());
    ASSERT_TRUE(text_of(out, "activity").empty());
    ASSERT_TRUE(text_of(out, "mcp").empty());
}

TEST(core_segments_show_one_job_singular_and_seconds) {
    auto registry = core_registry();
    StatusSnapshot one = base_snapshot();
    one.running_jobs = 1;
    one.job_seconds_left = 42;
    ASSERT_EQ(text_of(registry.render(one), "activity"),
              std::string("  1 job 42s"));

    StatusSnapshot many = base_snapshot();
    many.running_jobs = 3;
    many.job_seconds_left = -1;
    ASSERT_EQ(text_of(registry.render(many), "activity"),
              std::string("  3 jobs"));
}

TEST(core_segments_prefer_a_running_tool_over_idle) {
    auto registry = core_registry();
    StatusSnapshot tool = base_snapshot();
    tool.running_tool = "search";
    ASSERT_EQ(text_of(registry.render(tool), "activity"), std::string("  search\u2026"));
}

TEST(core_segments_summarize_mcp_servers) {
    auto registry = core_registry();
    StatusSnapshot mcp = base_snapshot();
    mcp.mcp_servers = {{"github", true, false},
                       {"broken", false, true},
                       {"silent", false, false}};  // not connected, no error: hidden

    ASSERT_EQ(text_of(registry.render(mcp), "mcp"),
              std::string("  mcp: github\u00b7!broken"));
}

TEST(core_segments_show_scroll_mode_only_when_on) {
    auto registry = core_registry();
    StatusSnapshot scrolling = base_snapshot();
    scrolling.scroll_mode = true;
    auto out = registry.render(scrolling);
    ASSERT_EQ(text_of(out, "scroll"), std::string(" S "));
    for (const auto& s : out)
        if (s.id == "scroll") ASSERT_EQ(s.drop_priority, 0);  // never dropped
}

TEST(core_segments_escalate_lag_tone) {
    auto registry = core_registry();

    StatusSnapshot slow = base_snapshot();
    slow.latency_ms = 7000;
    for (const auto& s : registry.render(slow))
        if (s.id == "lag") ASSERT_TRUE(s.tone == StatusTone::Crit);

    StatusSnapshot warm = base_snapshot();
    warm.latency_ms = 2000;
    for (const auto& s : registry.render(warm))
        if (s.id == "lag") ASSERT_TRUE(s.tone == StatusTone::Warn);

    StatusSnapshot fast = base_snapshot();
    fast.latency_ms = 100;
    for (const auto& s : registry.render(fast))
        if (s.id == "lag") ASSERT_TRUE(s.tone == StatusTone::Dim);
}

TEST(status_segment_capability_installs_and_unwinds) {
    ToolRegistry tools;
    PromptRegistry prompts;
    CommandRegistry commands;
    StatusRegistry status;
    PluginSettingsStore settings;
    EventBus bus;
    PluginServices services(tools, prompts, commands, status, settings, bus);
    services.set_owner("gemini");

    StatusSegmentCapability cap("balance", 850, 4, [](const StatusSnapshot&) {
        return StatusText{"$3.50", StatusTone::Good};
    });
    InstallResult r = cap.install(services);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(status.size(), 1u);

    auto out = status.render(StatusSnapshot{});
    ASSERT_EQ(out.size(), 1u);
    ASSERT_EQ(out[0].text, std::string("$3.50"));
    ASSERT_EQ(out[0].drop_priority, 4);

    r.contribution.remove();
    ASSERT_EQ(status.size(), 0u);
    ASSERT_TRUE(status.render(StatusSnapshot{}).empty());
}
