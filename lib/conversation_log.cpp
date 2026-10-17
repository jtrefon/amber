
#include "agent/conversation_log.h"
#include <chrono>
#include <filesystem>

namespace agent {

namespace {

long long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// Rotate rather than grow without bound: the journal is for what just went
// wrong, not an archive.
constexpr std::size_t kMaxErrorBytes = 1024UL * 1024UL;

// The journal holds what the APP could not do. A failed tool call is the model's
// business and stays in the per-session transcript only.
bool is_app_failure(const std::string& type) {
    return type == "error" || type == "chat_error";
}

} // namespace

void ConversationLog::open(const std::string& path) {
    if (path.empty())
        return;
    session_ = std::to_string(now_ms());
    std::string resolved = path;
    const std::string tok = "{ts}";
    size_t pos = resolved.find(tok);
    if (pos != std::string::npos)
        resolved.replace(pos, tok.size(), session_);
    // The default path lives under .amber/logs/, which may not exist yet.
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(resolved).parent_path(), ec);
    out_.open(resolved, std::ios::app);
    if (out_.is_open())
        event("session_start", {{"session", session_}});
}

void ConversationLog::open_errors(const std::string& path) {
    if (path.empty() || errors_.is_open())
        return;
    errors_path_ = path;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    if (std::filesystem::exists(path, ec)) {
        const auto size = std::filesystem::file_size(path, ec);
        errors_bytes_ = ec ? 0 : static_cast<std::size_t>(size);
    }
    errors_.open(path, std::ios::app);
}

void ConversationLog::rotate_errors() {
    if (errors_bytes_ <= kMaxErrorBytes)
        return;
    errors_.close();
    std::error_code ec;
    std::filesystem::rename(errors_path_, errors_path_ + ".1", ec);
    errors_bytes_ = 0;
    errors_.open(errors_path_, std::ios::trunc);
}

void ConversationLog::record_error(const std::string& type, const json& fields) {
    if (!errors_.is_open())
        return;
    json rec = fields;
    rec["ts"] = now_ms();
    rec["session"] = session_;
    rec["event"] = type;
    const std::string line = rec.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
    errors_ << line;
    errors_.flush();
    errors_bytes_ += line.size();
    rotate_errors();
}

void ConversationLog::event(const std::string& type, const json& fields) {
    if (is_app_failure(type))
        record_error(type, fields);
    if (!out_.is_open())
        return;
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
