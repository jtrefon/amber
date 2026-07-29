// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AGENT_CONVERSATION_LOG_H
#define AGENT_CONVERSATION_LOG_H

#include <string>
#include <fstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace agent {

// Appends conversation events to a JSON Lines file for telemetry/audit. Each
// call writes one self-contained JSON object with a unix-millisecond timestamp
// and a session id. No-op (and cheap) when logging is disabled.
class ConversationLog {
public:
    // path may contain "{ts}", replaced with the session start timestamp.
    void open(const std::string& path);
    bool enabled() const { return out_.is_open(); }


    // event: "session_start", "user", "assistant", "reasoning",
    //        "tool_call", "tool_result", "error", "session_end".
    void event(const std::string& type, const json& fields);

private:
    std::ofstream out_;
    std::string session_;
};

} // namespace agent

#endif
