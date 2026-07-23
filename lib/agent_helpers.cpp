// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/agent_helpers.h"
#include "agent/tool_call_parser.h"
#include "agent/llm.h"
#include "agent/tool.h"

#include <cctype>
#include <functional>
#include <stdexcept>

namespace agent {

std::string format_tool_envelope(const std::string& name, const json& args,
                                  const ToolResult& result) {
    std::string status = result.ok ? "ok" : "error";

    // Ensure meta is always an object, never null (tools that return early
    // on error may leave meta uninitialized).
    json meta = result.meta.is_null() ? json::object() : result.meta;

    std::string header = "[tool=" + name +
        " args=" + args.dump() +
        " status=" + status +
        " meta=" + meta.dump() + "]\n";

    std::string content = result.ok
        ? result.output
        : ("ERROR: " + result.error);
    if (!content.empty() && content.back() != '\n')
        content += '\n';

    return header + content + "[end]";
}

std::string strip_think(std::string s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (s.compare(i, 7, "<think>") == 0) {
            size_t close = s.find("</think>", i + 7);
            if (close == std::string::npos) break;  // truncated; drop trailing
            i = close + 8;
            continue;
        }
        out += s[i++];
    }
    return out;
}

std::string utf8_sanitize(std::string s) {
    std::string out;
    out.reserve(s.size());
    auto is_cont = [](unsigned char c) { return (c & 0xC0) == 0x80; };
    for (size_t i = 0; i < s.size();) {
        auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) { out += c; ++i; continue; }
        int n = 0;
        if ((c >> 5) == 0x6) n = 2;
        else if ((c >> 4) == 0xE) n = 3;
        else if ((c >> 3) == 0x1E) n = 4;
        if (n == 0) { out += '\xEF'; out += '\xBF'; out += '\xBD'; ++i; continue; }
        bool ok = true;
        for (int k = 1; k < n; ++k) {
            if (i + k >= s.size() || !is_cont(static_cast<unsigned char>(s[i + k]))) {
                ok = false; break;
            }
        }
        if (!ok) { out += '\xEF'; out += '\xBF'; out += '\xBD'; ++i; continue; }
        out.append(s, i, n);
        i += n;
    }
    return out;
}

void parse_tool_call(const json& call, std::string& id, std::string& fn,
                     json& args, bool& ok) {
    ok = true;
    auto str_or = [](const json& j, const char* k,
                     const std::string& d) -> std::string {
        auto it = j.find(k);
        return (it != j.end() && it->is_string()) ? it->get<std::string>() : d;
    };
    json fnobj = call.contains("function") && call["function"].is_object()
                     ? call["function"] : json::object();
    id = str_or(call, "id", "");
    fn = str_or(fnobj, "name", "");
    args = fnobj.contains("arguments") && !fnobj["arguments"].is_null()
                ? fnobj["arguments"] : json::object();
    if (args.is_string()) {
        try { args = json::parse(args.get<std::string>()); }
        catch (...) { ok = false; }
    }
}

bool maybe_extract_text_tool_calls(json& tool_calls, std::string& content,
                                   Message& stored, const AgentHooks& hooks) {
    bool has_json = !tool_calls.is_null() && !tool_calls.empty();
    if (has_json || content.empty()) return false;
    auto extracted = extract_tool_calls_from_text(content);
    if (extracted.is_null()) return false;
    tool_calls = std::move(extracted);
    content.clear();
    stored.content.clear();
    stored.tool_calls = tool_calls;
    if (hooks.on_status) hooks.on_status("parsed tool calls from text");
    return true;
}

Message safe_chat_once(const AgentHooks& hooks, ConversationLog& log,
                       const std::function<Message()>& chat, const char* stage) {
    try {
        Message m = chat();
        m.content = utf8_sanitize(m.content);
        m.reasoning = utf8_sanitize(m.reasoning);
        return m;
    } catch (const std::exception& e) {
        log.event("chat_error", {{"stage", stage}, {"error", e.what()}});
        if (hooks.on_status)
            hooks.on_status(std::string("chat error (recovered): ") + e.what());
        if (hooks.on_debug)
            hooks.on_debug("chat error: " + std::string(e.what()));
        Message err;
        err.role = "assistant";
        err.content = "[error during " + std::string(stage) +
                      ": " + e.what() +
                      "] Please retry or adjust your approach.";
        return err;
    }
}

std::string empty_turn_reply(const std::vector<Message>& history) {
    bool had_tool = false;
    for (const auto& m : history)
        if (m.role == "tool") { had_tool = true; break; }
    return had_tool
        ? "[agent stopped: the model stopped producing usable output after "
          "tool calls; see the ERROR messages above]"
        : "[agent stopped: the model produced no usable response]";
}

} // namespace agent
