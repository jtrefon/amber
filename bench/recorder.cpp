
#include "bench/recorder.h"

#include <chrono>

namespace bench {

namespace {

long parse_attempt(const std::string& text) noexcept {
    size_t open = text.find("retrying (");
    if (open == std::string::npos) return 0;
    open += 10;
    size_t end = text.find('/', open);
    if (end == std::string::npos) return 0;
    try {
        return std::stol(text.substr(open, end - open));
    } catch (...) {
        return 0;
    }
}

long parse_iteration(const std::string& text) noexcept {
    size_t pos = text.find("iteration ");
    if (pos == std::string::npos) return 0;
    pos += 10;
    size_t end = text.find('/', pos);
    if (end == std::string::npos) return 0;
    try {
        return std::stol(text.substr(pos, end - pos));
    } catch (...) {
        return 0;
    }
}

} // namespace

agent::AgentHooks Recorder::hooks() {
    agent::AgentHooks h;
    h.on_tool_call = [this](const std::string& n, const agent::json& a) {
        on_tool_call(n, a);
    };
    h.on_tool_result = [this](const std::string& n, const agent::ToolResult& r,
                              const agent::json& a) {
        on_tool_result(n, r, a);
    };
    h.on_status = [this](const std::string& t) { on_status(t); };
    h.on_debug = [this](const std::string& t) { on_debug(t); };
    h.on_stats = [this](const agent::Stats& s) { on_stats(s); };
    h.on_state = [this](agent::RunState st) { on_state(st); };
    return h;
}

void Recorder::on_tool_call(const std::string& name, const agent::json& args) {
    const long t = now_ms();
    pending_.push_back({fingerprint(name, args), name, args, t});
    stream_.calls.push_back({name, args, t, ""});
}

void Recorder::on_tool_result(const std::string& name,
                              const agent::ToolResult& res,
                              const agent::json&) {
    ToolEvent e;
    e.name = name;
    e.ok = res.ok;
    e.error = res.error;
    if (res.meta.is_object()) {
        e.denied = res.meta.value("denied", false);
        e.timeout = res.meta.value("timeout", false);
    }
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
        if (it->name != name) continue;
        e.args = it->args;
        e.duration_ms = now_ms() - it->t0_ms;
        pending_.erase(it);
        break;
    }
    for (auto& c : stream_.calls) {
        if (c.name != name || !c.status.empty()) continue;
        if (e.ok)
            c.status = "ok";
        else if (e.denied)
            c.status = "denied";
        else
            c.status = "error";
        break;
    }
    stream_.tools.push_back(std::move(e));
}

void Recorder::on_status(const std::string& text) { parse_status(text, stream_); }

void Recorder::on_debug(const std::string& text) { parse_debug(text, stream_); }

void Recorder::on_stats(const agent::Stats& s) {
    StatsEvent e;
    e.latency_ms = s.latency_ms;
    e.tps = s.tps;
    e.prompt_tokens = s.prompt_tokens > 0 ? s.prompt_tokens : 0;
    e.completion_tokens = s.completion_tokens > 0 ? s.completion_tokens : 0;
    stream_.stats.push_back(e);
    stream_.prompt_tokens += e.prompt_tokens;
    stream_.completion_tokens += e.completion_tokens;
    if (stream_.ttft_ms < 0 && s.latency_ms >= 0) stream_.ttft_ms = s.latency_ms;
}

void Recorder::on_state(agent::RunState) {}

std::string Recorder::fingerprint(const std::string& name,
                                  const agent::json& args) noexcept {
    return name + "|" +
           (args.is_string() ? args.get<std::string>() : args.dump());
}

long Recorder::now_ms() noexcept {
    return static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void parse_status(const std::string& text, EventStream& out) noexcept {
    if (text.find("compressing ") != std::string::npos) {
        ++out.compressions;
    } else if (text.find("LLM error - retrying (") != std::string::npos) {
        out.retries.push_back({static_cast<int>(parse_attempt(text))});
    } else if (text.find("LLM request repaired, retrying") != std::string::npos) {
        out.recoveries.push_back({"repaired"});
    } else if (text.find("tool recovery: injected steer") != std::string::npos ||
               text.find("text loop: injected recovery steer") != std::string::npos ||
               text.find("loop detected: breaking tool loop") != std::string::npos) {
        out.recoveries.push_back({"steer"});
    } else if (text.find("server rejected model") != std::string::npos) {
        out.recoveries.push_back({"model"});
    } else if (text.find("tool recovery failed, stopping") != std::string::npos ||
               text.find("agent looped beyond recovery, stopping") != std::string::npos) {
        out.hard_stop = true;
    }
}

void parse_debug(const std::string& text, EventStream& out) noexcept {
    long it = parse_iteration(text);
    if (it > 0 && it > out.iterations) out.iterations = static_cast<int>(it);
}

} // namespace bench
