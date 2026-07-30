
#include "agent/conversation_log.h"
#include <chrono>

namespace agent {

namespace {
long long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
        .count();
}
} // namespace

void ConversationLog::open(const std::string& path) {
    if (path.empty()) return;
    session_ = std::to_string(now_ms());
    std::string resolved = path;
    const std::string tok = "{ts}";
    size_t pos = resolved.find(tok);
    if (pos != std::string::npos)
        resolved.replace(pos, tok.size(), session_);
    out_.open(resolved, std::ios::app);
    if (out_.is_open())
        event("session_start", {{"session", session_}});
}

void ConversationLog::event(const std::string& type, const json& fields) {
    if (!out_.is_open()) return;
    json rec = fields;
    rec["ts"] = now_ms();
    rec["session"] = session_;
    rec["event"] = type;
    // Model output can contain invalid UTF-8 (e.g. a multibyte char split
    // across stream fragments); replace bad bytes instead of throwing.
    out_ << rec.dump(-1, ' ', false, json::error_handler_t::replace) << '\n';
    out_.flush();
}

} // namespace agent
