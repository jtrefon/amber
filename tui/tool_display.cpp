
#include "tui/tool_display.h"

#include <algorithm>
#include <cstdio>
#include <utility>

#include "agent/statusbar.h"
#include "agent/workspace.h"
#include "tui/textutil.h"

namespace tui::tool_display {

namespace {

constexpr size_t kCommandCap = 160;
constexpr size_t kTaskCap = 40;

// Tool args may carry absolute workspace paths; relativize them for display
// so a status line stays short enough to render on a single row.
std::string display_path(const std::string& p) {
    return agent::Workspace::relative(p);
}

// Tool name -> working-indicator verb, in declaration order.
constexpr std::pair<const char*, const char*> kToolVerbs[] = {
    {"bash", "hacking"},
    {"list_skills", "consulting"},
    {"process_read", "reading"},
    {"process_start", "spawning"},
    {"process_stop", "stopping"},
    {"read", "reading"},
    {"read_skill", "consulting"},
    {"search", "searching"},
    {"task", "delegating"},
    {"todowrite", "planning"},
    {"write", "writing"},
    {"write_skill", "authoring"},
};

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

std::string activity_verb(bool compressing, agent::RunState state,
                          const std::string& running_tool) {
    if (compressing) return "compressing";
    if (!running_tool.empty()) {
        for (const auto& [name, verb] : kToolVerbs)
            if (running_tool == name) return verb;
        if (running_tool.rfind("mcp_", 0) == 0) return "calling";
        return "working";
    }
    switch (state) {
        case agent::RunState::Thinking:  return "thinking";
        case agent::RunState::Streaming: return "talking";
        case agent::RunState::Waiting:   return "waiting";
        case agent::RunState::Error:     return "retrying";
        default:                         return "working";
    }
}

std::string describe_tool_call(const std::string& name,
                               const agent::json& args) {
    if (name == "bash") {
        // The command IS the story — no tool name, full params and paths.
        std::string cmd = arg(args, "command");
        if (!cmd.empty()) return truncate(cmd, kCommandCap);
    } else if (name == "read" || name == "write") {
        std::string path = arg(args, "path");
        if (!path.empty()) return name + " " + display_path(path);
    } else if (name == "search") {
        std::string pattern = arg(args, "pattern");
        if (!pattern.empty()) {
            std::string path = arg(args, "path");
            return path.empty() ? "search " + pattern
                                : "search " + pattern + " in " +
                                      display_path(path);
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

std::string working_label(const std::string& frame, const std::string& verb,
                          size_t elapsed_secs, const std::string& task) {
    std::string out = frame + " " + verb + " " + elapsed_label(elapsed_secs);
    if (!task.empty())
        out += " \u00b7 " + truncate(task, kTaskCap);
    return out;
}

std::string reasoning_badge(const std::string& effort) {
    return agent::bar::reasoning_badge(effort);
}

rich::Line result_line(const std::string& name, const agent::json& args,
                       bool ok, const std::string& output,
                       const std::string& error) {
    rich::Line ln;
    rich::Run icon;
    icon.pair = ok ? P_GIT_PLUS : P_GIT_MINUS;
    icon.text = ok ? text::glyph::check() : text::glyph::cross();
    rich::Run rest;
    rest.pair = P_STATUS;
    rest.text = " " + describe_tool_call(name, args) + "  " +
                text::glyph::arrow() + " ";
    if (!ok) {
        rest.text += "error: " + error;
    } else {
        int lines = 1;
        for (char c : output)
            if (c == '\n') ++lines;
        std::string preview = output;
        size_t nl = preview.find('\n');
        if (nl != std::string::npos) preview.resize(nl);
        if (preview.size() > 60) {
            preview.resize(57);
            preview += "...";
        }
        rest.text += "(" + std::to_string(lines) + " lines)  " + preview;
    }
    ln.runs.push_back(std::move(icon));
    ln.runs.push_back(std::move(rest));
    return ln;
}

long gauge_tokens(long server_prompt_tokens, long estimate, long streamed) {
    // A non-positive server count is degenerate/unknown — fall back to the
    // live estimate (same > 0 convention as the compression gate).
    long base = server_prompt_tokens > 0 ? server_prompt_tokens : estimate;
    return base + streamed;
}

} // namespace tui::tool_display
