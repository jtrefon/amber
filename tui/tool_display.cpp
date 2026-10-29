
#include "tui/tool_display.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <utility>

#include "agent/compact.h"
#include "agent/registry.h"
#include "agent/statusbar.h"
#include "agent/workspace.h"
#include "tui/textutil.h"

namespace tui::tool_display {

namespace {

constexpr size_t kCommandCap = 160;
constexpr size_t kTaskCap = 40;
constexpr size_t kIntentCap = 60;
constexpr size_t kStuckSeconds = 60;
// A tool's own summary gets the same room as a command line.
constexpr size_t kSummaryCap = 160;

// Tool args may carry absolute workspace paths; relativize them for display
// so a status line stays short enough to render on a single row.
std::string display_path(const std::string& p) {
    return agent::Workspace::relative(p);
}

std::string truncate(const std::string& s, size_t cap) {
    if (s.size() <= cap)
        return s;
    return s.substr(0, cap - 1) + "\u2026";
}

std::string trim_ws(const std::string& in) {
    std::size_t begin = 0;
    std::size_t end = in.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(in[begin])))
        ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(in[end - 1])))
        --end;
    return in.substr(begin, end - begin);
}

std::string strip_list_marker(std::string line) {
    while (line.size() >= 2 &&
           (line[0] == '-' || line[0] == '*' || line[0] == '+' || line[0] == '>') && line[1] == ' ')
        line.erase(0, 2);
    std::size_t digits = 0;
    while (digits < line.size() && std::isdigit(static_cast<unsigned char>(line[digits])))
        ++digits;
    if (digits > 0 && digits + 1 < line.size() && line[digits] == '.' && line[digits + 1] == ' ')
        line.erase(0, digits + 2);
    return line;
}

std::string strip_inline_emphasis(const std::string& line) {
    std::string out;
    out.reserve(line.size());
    std::size_t i = 0;
    while (i < line.size()) {
        const char c = line[i];
        if (c == '`') {
            ++i;
            continue;
        }
        if ((c == '*' || c == '_') && i + 1 < line.size() && line[i + 1] == c) {
            i += 2;
            continue;
        }
        out += c;
        ++i;
    }
    return out;
}

std::string collapse_whitespace(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    bool pending_space = false;
    for (char c : in) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            pending_space = true;
            continue;
        }
        if (pending_space && !out.empty())
            out += ' ';
        pending_space = false;
        out += c;
    }
    return trim_ws(out);
}

std::string strip_inline_markdown(std::string line) {
    return collapse_whitespace(strip_inline_emphasis(strip_list_marker(std::move(line))));
}

std::string strip_intent_lead(std::string line) {
    static const char* const kLeads[] = {"let me ", "i'll ", "i will ", "i'm going to ",
                                         "i am going to "};
    for (const char* lead : kLeads) {
        const std::string prefix = lead;
        if (line.size() <= prefix.size())
            continue;
        std::string head = line.substr(0, prefix.size());
        for (char& c : head)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (head != prefix)
            continue;
        const std::string rest = trim_ws(line.substr(prefix.size()));
        if (rest.size() >= 8)
            return rest;
    }
    return line;
}

std::string arg(const agent::json& args, const char* key) {
    if (args.is_object() && args.contains(key) && args[key].is_string())
        return args[key].get<std::string>();
    return {};
}

// The four tools whose call reads better in our words than in the tool's own. They keep
// bespoke rendering because it is terser than wording written for an approval prompt; a
// tool we have never heard of still falls back to its own summarize(), which is the only
// thing that can describe it.
//
// Returns empty when this is not one of the four, or when the argument the rendering
// needs is absent — which is how the caller falls through.
std::string render_known_tool(const std::string& name, const agent::json& args) {
    if (name == "bash") {
        // The command IS the story - no tool name, full params and paths.
        const std::string cmd = arg(args, "command");
        if (!cmd.empty())
            return truncate(cmd, kCommandCap);
        return {};
    }
    if (name == "read" || name == "write") {
        const std::string path = arg(args, "path");
        if (!path.empty())
            return name + " " + display_path(path);
        return {};
    }
    if (name == "search") {
        const std::string pattern = arg(args, "pattern");
        if (pattern.empty())
            return {};
        const std::string path = arg(args, "path");
        return path.empty() ? "search " + pattern
                            : "search " + pattern + " in " + display_path(path);
    }
    return {};
}

} // namespace

std::string activity_verb(bool compressing, agent::RunState state, const std::string& running_tool,
                          const agent::ToolRegistry& registry) {
    if (compressing)
        return "compressing";
    if (!running_tool.empty()) {
        // The tool owns its word (declared with its registration); a tool that
        // declared none still gets MCP's convention or the generic fallback.
        std::string verb = registry.meta_for(running_tool).verb;
        if (!verb.empty())
            return verb;
        if (running_tool.rfind("mcp_", 0) == 0)
            return "calling";
        return "working";
    }
    switch (state) {
    case agent::RunState::Thinking:
        return "thinking";
    case agent::RunState::Streaming:
        return "talking";
    case agent::RunState::Waiting:
        return "waiting";
    case agent::RunState::Error:
        return "retrying";
    default:
        return "working";
    }
}

std::string describe_tool_call(const std::string& name, const agent::json& args,
                               const agent::ToolRegistry& registry) {
    if (std::string bespoke = render_known_tool(name, args); !bespoke.empty())
        return bespoke;
    // A tool that describes its own invocation does it better than we can —
    // and it is the only thing that CAN, for a tool we have never heard of.
    // The four shapes above keep their bespoke rendering: they are terser than
    // the tool's own wording, which is written for approval prompts.
    if (auto tool = registry.find(name)) {
        const std::string summary = tool->summarize(args);
        if (!summary.empty() && summary != name)
            return truncate(summary, kSummaryCap);
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
    if (secs == 0)
        return "0s";
    return agent::compact::duration_ms(static_cast<long>(secs) * 1000);
}

WorkingParts working_parts(const std::string& frame, const std::string& verb, size_t elapsed_secs,
                           const std::string& task, const std::string& intent) {
    WorkingParts parts;
    parts.glyph = frame;
    parts.verb = verb;
    parts.tail = " " + elapsed_label(elapsed_secs);
    if (!intent.empty())
        parts.tail += " \u00b7 " + truncate(intent, kIntentCap);
    if (!task.empty())
        parts.tail += " \u00b7 " + truncate(task, kTaskCap);
    return parts;
}

std::string working_label(const std::string& frame, const std::string& verb, size_t elapsed_secs,
                          const std::string& task, const std::string& intent) {
    const WorkingParts parts = working_parts(frame, verb, elapsed_secs, task, intent);
    return parts.glyph + " " + parts.verb + parts.tail;
}

std::string intent_from_narration(const std::string& text) {
    std::size_t end = text.size();
    while (end > 0) {
        const std::size_t newline = text.rfind('\n', end - 1);
        const std::size_t start = newline == std::string::npos ? 0 : newline + 1;
        const std::string raw = trim_ws(text.substr(start, end - start));
        if (raw.rfind("```", 0) == 0)
            return {};
        if (!raw.empty() && raw[0] != '#' && raw.rfind("---", 0) != 0 && raw.rfind("===", 0) != 0 &&
            raw.rfind('|', 0) != 0) {
            const std::string line = strip_intent_lead(strip_inline_markdown(raw));
            if (line.size() >= 3 && line != "..." && line != "\u2026")
                return truncate(line, kIntentCap);
        }
        if (newline == std::string::npos)
            break;
        end = newline;
    }
    return {};
}

std::string stuck_hint(size_t elapsed_secs) {
    if (elapsed_secs < kStuckSeconds)
        return {};
    return "no progress for " + std::to_string(kStuckSeconds) + "s \u2014 Ctrl+C to cancel";
}

std::string reasoning_badge(const std::string& effort) {
    return agent::bar::reasoning_badge(effort);
}

rich::Line result_line(const std::string& name, const agent::json& args, bool ok,
                       const std::string& output, const std::string& error,
                       const agent::ToolRegistry& registry) {
    rich::Line ln;
    rich::Run icon;
    icon.pair = ok ? P_GIT_PLUS : P_GIT_MINUS;
    icon.text = ok ? text::glyph::check() : text::glyph::cross();
    rich::Run rest;
    rest.pair = P_STATUS;
    rest.text = " " + describe_tool_call(name, args, registry) + "  " + text::glyph::arrow() + " ";
    if (!ok) {
        // The line is a signpost, not the report: an error dump here would
        // rewrap into rows the user has to scroll past. The full text is in the
        // tool's own output and the conversation log.
        std::string shown = error;
        if (shown.size() > kCommandCap)
            shown.replace(kCommandCap, std::string::npos, "...");
        rest.text += "error: " + shown;
    } else {
        int lines = 1;
        for (char c : output)
            if (c == '\n')
                ++lines;
        std::string preview = output;
        size_t nl = preview.find('\n');
        if (nl != std::string::npos)
            preview.resize(nl);
        if (preview.size() > 60) {
            preview.resize(57);
            preview += "...";
        }
        rest.text += "(" + agent::compact::count(lines) + " lines)  " + preview;
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
