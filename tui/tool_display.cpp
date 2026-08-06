
#include "tui/tool_display.h"

#include <algorithm>
#include <cstdio>

#include "tui/textutil.h"

namespace tui::tool_display {

namespace {

constexpr size_t kCommandCap = 160;
constexpr size_t kTaskCap = 40;

std::string truncate(const std::string& s, size_t cap) {
    if (s.size() <= cap) return s;
    return s.substr(0, cap - 1) + "\u2026";
}

std::string arg(const agent::json& args, const char* key) {
    if (args.is_object() && args.contains(key) && args[key].is_string())
        return args[key].get<std::string>();
    return {};
}

} // namespace

std::string describe_tool_call(const std::string& name,
                               const agent::json& args) {
    if (name == "bash") {
        // The command IS the story — no tool name, full params and paths.
        std::string cmd = arg(args, "command");
        if (!cmd.empty()) return truncate(cmd, kCommandCap);
    } else if (name == "read" || name == "write") {
        std::string path = arg(args, "path");
        if (!path.empty()) return name + " " + path;
    } else if (name == "search") {
        std::string pattern = arg(args, "pattern");
        if (!pattern.empty()) {
            std::string path = arg(args, "path");
            return path.empty() ? "search " + pattern
                                : "search " + pattern + " in " + path;
        }
    }
    // Generic fallback: name + truncated raw args (unchanged behaviour).
    std::string d = name;
    if (args.is_object() && !args.empty()) {
        std::string dump = args.dump();
        if (dump.size() > 60) {
            dump.resize(57);
            dump += "...";
        }
        d += " " + dump;
    }
    return d;
}

rich::Line close_tool_line(const rich::Line& open, rich::Line summary) {
    // The open line's first run is the faint timestamp (dim P_REASONING);
    // keep it on the closed line so the single line stays timestamped.
    if (!open.runs.empty()) {
        const rich::Run& ts = open.runs[0];
        if (ts.dim && ts.pair == P_REASONING)
            summary.runs.insert(summary.runs.begin(), ts);
    }
    return summary;
}

std::string elapsed_label(size_t secs) {
    char b[32];
    if (secs < 60)
        std::snprintf(b, sizeof(b), "%zus", secs);
    else if (secs < 3600)
        std::snprintf(b, sizeof(b), "%zum %02zus", secs / 60, secs % 60);
    else
        std::snprintf(b, sizeof(b), "%zuh %02zum", secs / 3600,
                      (secs % 3600) / 60);
    return b;
}

std::string working_label(const std::string& frame, size_t elapsed_secs,
                          const std::string& task) {
    std::string out = frame + " working " + elapsed_label(elapsed_secs);
    if (!task.empty())
        out += " \u00b7 " + truncate(task, kTaskCap);
    return out;
}

std::string reasoning_badge(const std::string& effort) {
    return "(" + effort + ")";
}

} // namespace tui::tool_display
