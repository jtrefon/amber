
#include "agent/agent_helpers.h"
#include "agent/tool_call_parser.h"
#include "agent/llm.h"
#include "agent/tool.h"

#include <cctype>
#include <functional>
#include <stdexcept>
#include <unistd.h>

namespace agent {

std::string fingerprint_tool_calls(const json& calls) {
    if (calls.is_null() || !calls.is_array() || calls.empty())
        return {};
    std::string key;
    for (const auto& tc : calls) {
        std::string id, fn;
        json args;
        bool ok = true;
        parse_tool_call(tc, id, fn, args, ok);
        if (!key.empty()) key += '|';
        key += fn;
        key += '|';
        key += args.dump();
    }
    return key;
}

std::string format_tool_envelope(const std::string& name, const json& args,
                                  const ToolResult& result) {
    // Ensure meta is always an object, never null (tools that return early
    // on error may leave meta uninitialized).
    json meta = result.meta.is_null() ? json::object() : result.meta;

    std::string status;
    if (result.ok) {
        status = "ok";
    } else if (meta.value("denied", false)) {
        status = "denied";
    } else if (meta.value("timeout", false)) {
        status = "timeout";
    } else {
        status = "error";
    }

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

namespace {
bool retryable_error(const std::exception& e) {
    const auto* api = dynamic_cast<const ApiError*>(&e);
    return api == nullptr || api->retryable;
}
int backoff_ms(int attempt) { return attempt <= 1 ? 1000 : 2000; }
bool wait_cancellable(const CancellationToken& token, int ms) {
    for (int waited = 0; waited < ms; waited += 100) {
        if (token.is_requested()) return true;
        usleep(100 * 1000);
    }
    return false;
}
} // namespace

Message chat_with_retry(const AgentHooks& hooks, ConversationLog& log,
                        const std::function<Message()>& chat,
                        const char* stage,
                        const CancellationToken& cancel_token,
                        int max_attempts) {
    std::string last_error;
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        try {
            Message m = chat();
            m.content = utf8_sanitize(m.content);
            m.reasoning = utf8_sanitize(m.reasoning);
            return m;
        } catch (const std::exception& e) {
            last_error = e.what();
            log.event("chat_error", {{"stage", stage},
                                     {"error", last_error},
                                     {"attempt", attempt}});
            if (hooks.on_debug)
                hooks.on_debug("chat error (attempt " +
                               std::to_string(attempt) + "): " + last_error);
            if (attempt >= max_attempts || !retryable_error(e)) break;
            if (hooks.on_status)
                hooks.on_status("LLM error - retrying (" +
                                std::to_string(attempt) + "/" +
                                std::to_string(max_attempts) + ") in " +
                                std::to_string(backoff_ms(attempt) / 1000) +
                                "s");
            if (wait_cancellable(cancel_token, backoff_ms(attempt))) {
                last_error = "cancelled by user";
                break;
            }
        }
    }
    Message err;
    err.role = "assistant";
    err.content = "[error during " + std::string(stage) + ": " +
                  last_error + "] Please retry or adjust your approach.";
    return err;
}


std::string empty_turn_reply(const std::deque<Message>& history) {
    bool had_tool = false;
    for (const auto& m : history)
        if (m.role == "tool") { had_tool = true; break; }
    return had_tool
        ? "[agent stopped: the model stopped producing usable output after "
          "tool calls; see the ERROR messages above]"
        : "[agent stopped: the model produced no usable response]";
}

} // namespace agent
