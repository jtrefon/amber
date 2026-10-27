#include "shape_replay.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "agent/agent_helpers.h"
#include "agent/compressor.h"
#include "nlohmann/json.hpp"

namespace bench {
namespace {

using json = nlohmann::json;

constexpr std::size_t kEnvelopeBudget = std::size_t{32} * 1024;
constexpr std::size_t kBigBash = std::size_t{8} * 1024;

// The envelope body: everything between the header line and the trailing
// "[end]" marker. Empty for a message that is already a digest.
std::string envelope_body(const std::string& content) {
    const std::size_t start = content.find('\n');
    if (start == std::string::npos)
        return {};
    const std::size_t end = content.rfind("[end]");
    if (end == std::string::npos || end < start)
        return content.substr(start + 1);
    return content.substr(start + 1, end - start - 1);
}

// The compact args the envelope echoes for small calls ("path", "pattern").
std::string header_field(const std::string& content, const std::string& key) {
    const std::size_t header_end = content.find('\n');
    const std::string header = content.substr(0, header_end);
    const std::string needle = "\"" + key + "\":\"";
    const std::size_t at = header.find(needle);
    if (at == std::string::npos)
        return {};
    const std::size_t from = at + needle.size();
    const std::size_t to = header.find('"', from);
    return to == std::string::npos ? std::string() : header.substr(from, to - from);
}

void append_tool_call(ReplayLog& log, const json& e) {
    json args = e.contains("args") && !e["args"].is_null() ? e["args"] : json::object();
    json call = {{"id", e.value("id", "")},
                 {"type", "function"},
                 {"function", {{"name", e.value("name", "")}, {"arguments", args.dump()}}}};
    const bool joins = !log.messages.empty() && log.messages.back().role == "assistant" &&
                       log.messages.back().content.empty() &&
                       log.messages.back().tool_calls.is_array();
    if (joins) {
        log.messages.back().tool_calls.push_back(std::move(call));
        return;
    }
    agent::Message m;
    m.role = "assistant";
    m.tool_calls = json::array({std::move(call)});
    log.messages.push_back(std::move(m));
}

void append_tool_result(ReplayLog& log, const json& e) {
    agent::ToolResult r;
    r.ok = e.value("ok", true);
    r.output = e.value("output", "");
    agent::Message m;
    m.role = "tool";
    m.name = e.value("name", "");
    m.tool_call_id = e.value("id", "");
    m.content = agent::utf8_sanitize(
        agent::format_tool_envelope(m.name, json::object(), r, kEnvelopeBudget));
    log.messages.push_back(std::move(m));
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            if (start < text.size())
                out.push_back(text.substr(start));
            break;
        }
        out.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool signal_line(const std::string& line) {
    static const char* kWords[] = {"error",     "fail",   "warn",      "errno",  "exception",
                                   "traceback", "panic",  "denied",    "fatal",  "total",
                                   "summary",   "passed", "not found", "cannot", "assert"};
    const std::string l = lower(line);
    return std::any_of(std::begin(kWords), std::end(kWords),
                       [&](const char* word) { return l.find(word) != std::string::npos; });
}

void replace_with_digest(agent::Message& m) {
    m.content = agent::tool_result_digest(m, "0000000000000000");
}

std::vector<std::size_t> rule_in_turn(std::vector<agent::Message>& msgs, std::size_t window) {
    agent::CompressionConfig cfg;
    cfg.context_size = static_cast<int>(window);
    std::vector<std::size_t> out = agent::in_turn_digest_candidates(msgs, cfg);
    for (const std::size_t i : out)
        replace_with_digest(msgs[i]);
    return out;
}

// Keep the envelope header, the first/last lines and every line that carries
// signal; drop the rest behind an omission marker. Candidate for CC-17.
bool compress_bash(agent::Message& m) {
    if (m.role != "tool" || m.name != "bash" || m.content.size() <= kBigBash)
        return false;
    const std::string body = envelope_body(m.content);
    if (body.size() <= kBigBash)
        return false;
    const std::vector<std::string> lines = split_lines(body);
    if (lines.size() < 8)
        return false;
    std::string kept;
    std::size_t omitted = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const bool edge = i < 5 || i + 10 >= lines.size();
        if (edge || signal_line(lines[i]))
            kept += lines[i] + "\n";
        else
            ++omitted;
    }
    if (omitted == 0 || kept.size() >= body.size())
        return false;
    const std::string header = m.content.substr(0, m.content.find('\n') + 1);
    m.content = header + kept + "[... " + std::to_string(omitted) + " lines omitted of " +
                std::to_string(lines.size()) + "; read_result for the rest]\n[end]";
    return true;
}

std::vector<std::size_t> rule_bash(std::vector<agent::Message>& msgs) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < msgs.size(); ++i)
        if (compress_bash(msgs[i]))
            out.push_back(i);
    return out;
}

std::vector<std::size_t> rule_supersede(std::vector<agent::Message>& msgs) {
    std::map<std::string, std::vector<std::size_t>> reads;
    for (std::size_t i = 0; i < msgs.size(); ++i) {
        if (msgs[i].role != "tool" || msgs[i].name != "read")
            continue;
        const std::string path = header_field(msgs[i].content, "path");
        if (!path.empty())
            reads[path].push_back(i);
    }
    std::vector<std::size_t> out;
    for (const auto& [path, indices] : reads) {
        (void)path;
        for (std::size_t k = 0; k + 1 < indices.size(); ++k)
            out.push_back(indices[k]);
    }
    std::sort(out.begin(), out.end());
    for (const std::size_t i : out)
        replace_with_digest(msgs[i]);
    return out;
}

std::vector<std::size_t> rule_drop_all(std::vector<agent::Message>& msgs) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < msgs.size(); ++i) {
        if (msgs[i].role != "tool")
            continue;
        replace_with_digest(msgs[i]);
        out.push_back(i);
    }
    return out;
}

// Lines worth looking for later: the head, the tail and every line carrying
// signal, deduplicated and capped. A run of one repeated character is filler
// (the fixtures use it), not a reference.
std::vector<std::string> distinctive_lines(const std::string& body) {
    const std::vector<std::string> lines = split_lines(body);
    std::vector<std::string> out;
    std::set<std::string> seen;
    auto add = [&](const std::string& line) {
        if (line.size() < 24 || out.size() >= 8 || !seen.insert(line).second)
            return;
        const bool filler =
            std::all_of(line.begin() + 8, line.end(), [&](char c) { return c == line[0]; });
        if (!filler)
            out.push_back(line);
    };
    for (std::size_t i = 0; i < lines.size() && i < 2; ++i)
        add(lines[i]);
    for (const std::string& line : lines)
        if (signal_line(line))
            add(line);
    for (std::size_t i = lines.size(); i > 0 && lines.size() - i < 2; --i)
        add(lines[i - 1]);
    return out;
}

std::size_t count_references(const std::vector<agent::Message>& before,
                             const std::vector<std::size_t>& replaced) {
    std::size_t hits = 0;
    for (const std::size_t i : replaced) {
        const std::vector<std::string> lines = distinctive_lines(envelope_body(before[i].content));
        if (lines.empty())
            continue;
        for (std::size_t j = i + 1; j < before.size(); ++j) {
            const std::string hay =
                before[j].content +
                (before[j].tool_calls.is_null() ? std::string() : before[j].tool_calls.dump());
            const bool named = std::any_of(lines.begin(), lines.end(), [&](const std::string& l) {
                return hay.find(l) != std::string::npos;
            });
            if (named) {
                ++hits;
                break;
            }
        }
    }
    return hits;
}

std::vector<std::size_t> apply_rule(std::vector<agent::Message>& msgs, const std::string& rule,
                                    std::size_t window) {
    if (rule == "in-turn")
        return rule_in_turn(msgs, window);
    if (rule == "supersede")
        return rule_supersede(msgs);
    if (rule == "drop-all")
        return rule_drop_all(msgs);
    return rule_bash(msgs);
}

} // namespace

ReplayLog parse_session_log(const std::string& path) {
    ReplayLog log;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        const json e = json::parse(line, nullptr, false);
        if (e.is_discarded() || !e.is_object())
            continue;
        const std::string type = e.value("event", "");
        if (type == "user" || type == "assistant") {
            agent::Message m;
            m.role = type;
            m.content = e.value("content", "");
            if (!m.content.empty())
                log.messages.push_back(std::move(m));
        } else if (type == "tool_call" && !log.messages.empty()) {
            append_tool_call(log, e);
        } else if (type == "tool_result") {
            append_tool_result(log, e);
        }
    }
    return log;
}

ShapeStats replay_rule(const ReplayLog& log, const std::string& rule, std::size_t window) {
    ShapeStats s;
    s.rule = rule;
    s.messages = log.messages.size();
    s.tokens_before = agent::estimate_tokens(log.messages);
    std::vector<agent::Message> msgs = log.messages;
    const std::vector<std::size_t> replaced = apply_rule(msgs, rule, window);
    s.tokens_after = agent::estimate_tokens(msgs);
    s.replaced = replaced.size();
    s.referenced = count_references(log.messages, replaced);
    return s;
}

std::string format_replay_report(const std::string& path, const std::vector<ShapeStats>& stats) {
    std::ostringstream out;
    out << path << "\n";
    out << "  rule         before   after   saved  replaced  referenced\n";
    for (const ShapeStats& s : stats) {
        const std::size_t saved = s.tokens_before > s.tokens_after
                                      ? 100 * (s.tokens_before - s.tokens_after) / s.tokens_before
                                      : 0;
        out << "  " << s.rule;
        for (std::size_t i = s.rule.size(); i < 13; ++i)
            out << ' ';
        out << s.tokens_before << "  " << s.tokens_after << "  " << saved << "%  " << s.replaced
            << "  " << s.referenced << "\n";
    }
    return out.str();
}

} // namespace bench
