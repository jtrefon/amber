#include "agent/core_segments.h"

#include "agent/statusbar.h"

#include <cstdio>
#include <string>

namespace agent {

namespace {

// Priorities leave gaps so a plugin can slot a segment between the built-ins
// without renumbering them.
enum Priority {
    kWindow = 100,
    kModel = 200,
    kMode = 300,
    kScroll = 400,
    kLag = 500,
    kTps = 600,
    kTokens = 700,
    kBalance = 800,
    kActivity = 900,
    kMcp = 1000,
};

// Drop priority is what goes first when the terminal is too narrow: the
// least-important segment has the highest number. Scroll mode never drops (0
// means "keep"), because the user asked for it explicitly.
enum Drop {
    kScrollDrop = 0,
    kActivityDrop = 1,
    kModeDrop = 2,
    kWindowDrop = 3,
    kTpsDrop = 4,
    kModelDrop = 5,
    kBalanceDrop = 5,
    kLagDrop = 6,
    kTokensDrop = 7,
    kMcpDrop = 8,
};

std::string dashed(const char* glyph) { return std::string(glyph); }

} // namespace

void register_core_status_segments(StatusRegistry& registry) {
    registry.add("", "window", kWindow, kWindowDrop, [](const StatusSnapshot& s) {
        return StatusText{"[" + std::to_string(s.window_index) + "/" +
                              std::to_string(s.window_count) + "]",
                          StatusTone::Banner};
    });

    registry.add("", "model", kModel, kModelDrop, [](const StatusSnapshot& s) {
        return StatusText{" [" + s.model +
                              bar::reasoning_badge(s.reasoning_effort) + "]",
                          StatusTone::Good};
    });

    registry.add("", "mode", kMode, kModeDrop, [](const StatusSnapshot& s) {
        switch (s.mode) {
            case AgentMode::Read:  return StatusText{" read ", StatusTone::Good};
            case AgentMode::Write: return StatusText{" write ", StatusTone::Warn};
            case AgentMode::Yolo:  return StatusText{" yolo ", StatusTone::Accent};
        }
        return StatusText{};
    });

    registry.add("", "scroll", kScroll, kScrollDrop, [](const StatusSnapshot& s) {
        if (!s.scroll_mode) return StatusText{};
        return StatusText{" S ", StatusTone::Good};
    });

    registry.add("", "lag", kLag, kLagDrop, [](const StatusSnapshot& s) {
        if (s.latency_ms < 0)
            return StatusText{"  lag " + dashed(bar::emdash()), StatusTone::Dim};
        char buf[32];
        std::snprintf(buf, sizeof(buf), "  lag %.0fms",
                      static_cast<double>(s.latency_ms));
        StatusTone tone = StatusTone::Dim;
        if (s.latency_ms > 5000) tone = StatusTone::Crit;
        else if (s.latency_ms > 1000) tone = StatusTone::Warn;
        return StatusText{buf, tone};
    });

    registry.add("", "tps", kTps, kTpsDrop, [](const StatusSnapshot& s) {
        if (s.tps <= 0.0)
            return StatusText{"  " + dashed(bar::emdash()) + " t/s", StatusTone::Dim};
        char buf[32];
        std::snprintf(buf, sizeof(buf), "  %.0f t/s", s.tps);
        return StatusText{buf, StatusTone::Dim};
    });

    registry.add("", "tokens", kTokens, kTokensDrop, [](const StatusSnapshot& s) {
        const std::string up = s.prompt_tokens >= 0 ? bar::kfmt(s.prompt_tokens)
                                                    : bar::emdash();
        const std::string down =
            s.completion_tokens >= 0 ? bar::kfmt(s.completion_tokens)
                                     : bar::emdash();
        return StatusText{"  " + dashed(bar::up()) + up + " " +
                              dashed(bar::down()) + down,
                          StatusTone::Dim};
    });

    // A provider's own readout (today the kilo.ai balance). A plugin that owns
    // such a readout registers its own segment instead; this one exists for the
    // built-in provider set until that conversion lands.
    registry.add("", "balance", kBalance, kBalanceDrop, [](const StatusSnapshot& s) {
        if (s.balance_label.empty()) return StatusText{};
        return StatusText{"  " + s.balance_label, StatusTone::Dim};
    });

    registry.add("", "activity", kActivity, kActivityDrop,
                 [](const StatusSnapshot& s) {
                     if (s.running_jobs > 0) {
                         std::string text = "  " + std::to_string(s.running_jobs) +
                                            " job" +
                                            (s.running_jobs > 1 ? "s" : "");
                         if (s.job_seconds_left >= 0)
                             text += " " + std::to_string(s.job_seconds_left) + "s";
                         return StatusText{std::move(text), StatusTone::Warn};
                     }
                     if (!s.running_tool.empty())
                         return StatusText{"  " + s.running_tool + "\u2026",
                                           StatusTone::Warn};
                     return StatusText{};
                 });

    registry.add("", "mcp", kMcp, kMcpDrop, [](const StatusSnapshot& s) {
        std::string text;
        for (const auto& server : s.mcp_servers) {
            if (!server.connected && !server.has_error) continue;
            if (!text.empty()) text += "\u00b7";
            text += (server.connected ? "" : "!") + server.name;
        }
        if (text.empty()) return StatusText{};
        return StatusText{"  mcp: " + text, StatusTone::Dim};
    });
}

} // namespace agent
