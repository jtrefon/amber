#include "agent/conversation_log.h"

#include <chrono>
#include <filesystem>
#include <system_error>

#include <sys/stat.h>
#include <unistd.h>

#include "agent/private_file.h"

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

ConversationLog::~ConversationLog() {
    if (out_fd_ >= 0)
        ::close(out_fd_);
    if (errors_fd_ >= 0)
        ::close(errors_fd_);
}

ConversationLog::ConversationLog(ConversationLog&& other) noexcept
    : out_fd_(other.out_fd_), errors_fd_(other.errors_fd_), session_(std::move(other.session_)),
      errors_path_(std::move(other.errors_path_)), errors_bytes_(other.errors_bytes_) {
    other.out_fd_ = -1;
    other.errors_fd_ = -1;
    other.errors_bytes_ = 0;
}

ConversationLog& ConversationLog::operator=(ConversationLog&& other) noexcept {
    if (this == &other)
        return *this;
    if (out_fd_ >= 0)
        ::close(out_fd_);
    if (errors_fd_ >= 0)
        ::close(errors_fd_);
    out_fd_ = other.out_fd_;
    errors_fd_ = other.errors_fd_;
    session_ = std::move(other.session_);
    errors_path_ = std::move(other.errors_path_);
    errors_bytes_ = other.errors_bytes_;
    other.out_fd_ = -1;
    other.errors_fd_ = -1;
    other.errors_bytes_ = 0;
    return *this;
}

void ConversationLog::open(const std::string& path) {
    if (path.empty())
        return;
    session_ = std::to_string(now_ms());
    std::string resolved = path;
    const std::string tok = "{ts}";
    size_t pos = resolved.find(tok);
    if (pos != std::string::npos)
        resolved.replace(pos, tok.size(), session_);
    out_fd_ = open_private_append(resolved);
    if (out_fd_ >= 0)
        event("session_start", {{"session", session_}});
}

void ConversationLog::open_errors(const std::string& path) {
    if (path.empty() || errors_fd_ >= 0)
        return;
    errors_path_ = path;
    errors_fd_ = open_private_append(path);
    if (errors_fd_ < 0)
        return;
    struct stat st {};
    if (::fstat(errors_fd_, &st) == 0)
        errors_bytes_ = static_cast<std::size_t>(st.st_size);
}

void ConversationLog::rotate_errors() {
    if (errors_bytes_ <= kMaxErrorBytes)
        return;
    if (errors_fd_ >= 0) {
        ::close(errors_fd_);
        errors_fd_ = -1;
    }
    std::error_code ec;
    std::filesystem::rename(errors_path_, errors_path_ + ".1", ec);
    errors_bytes_ = 0;
    errors_fd_ = open_private_truncate(errors_path_);
}

void ConversationLog::record_error(const std::string& type, const json& fields) {
    if (errors_fd_ < 0)
        return;
    json rec = fields;
    rec["ts"] = now_ms();
    rec["session"] = session_;
    rec["event"] = type;
    const std::string line = rec.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
    if (!write_all(errors_fd_, line))
        return;
    errors_bytes_ += line.size();
    rotate_errors();
}

void ConversationLog::event(const std::string& type, const json& fields) {
    if (is_app_failure(type))
        record_error(type, fields);
    if (out_fd_ < 0)
        return;
    json rec = fields;
    rec["ts"] = now_ms();
    rec["session"] = session_;
    rec["event"] = type;
    // Model output can contain invalid UTF-8 (e.g. a multibyte char split
    // across stream fragments); replace bad bytes instead of throwing.
    const std::string line = rec.dump(-1, ' ', false, json::error_handler_t::replace) + "\n";
    write_all(out_fd_, line);
}

} // namespace agent
