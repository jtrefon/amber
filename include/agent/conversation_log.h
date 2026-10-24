
#ifndef AGENT_CONVERSATION_LOG_H
#define AGENT_CONVERSATION_LOG_H

#include <string>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace agent {

// Appends conversation events to a JSON Lines file for telemetry/audit. Each
// call writes one self-contained JSON object with a unix-millisecond timestamp
// and a session id. No-op (and cheap) when logging is disabled.
class ConversationLog {
public:
    ConversationLog() = default;
    ~ConversationLog();
    ConversationLog(const ConversationLog&) = delete;
    ConversationLog& operator=(const ConversationLog&) = delete;
    ConversationLog(ConversationLog&& other) noexcept;
    ConversationLog& operator=(ConversationLog&& other) noexcept;

    // path may contain "{ts}", replaced with the session start timestamp.
    void open(const std::string& path);

    // The error journal: what the app could not do (chat errors, aborted turns,
    // uncaught failures) in one append-only file, rotated at kMaxErrorBytes with
    // one previous kept. Opened only when diagnostics are on — with them off
    // there is no journal, no buffer, and one bool test on the way.
    void open_errors(const std::string& path);
    bool errors_enabled() const { return errors_fd_ >= 0; }

    bool enabled() const { return out_fd_ >= 0; }

    // event: "session_start", "user", "assistant", "reasoning",
    //        "tool_call", "tool_result", "error", "session_end".
    void event(const std::string& type, const json& fields);

private:
    void record_error(const std::string& type, const json& fields);
    void rotate_errors();

    int out_fd_ = -1;
    int errors_fd_ = -1;
    std::string session_;
    std::string errors_path_;
    std::size_t errors_bytes_ = 0;
};

} // namespace agent

#endif
