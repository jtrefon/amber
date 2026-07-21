// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AGENT_TOOL_RECOVERY_H
#define AGENT_TOOL_RECOVERY_H

#include <map>
#include <string>

#include "agent/agent.h"  // AgentHooks, Message
#include "agent/conversation_log.h"
#include "agent/llm.h"  // json

namespace agent {

// Tracks tools that repeatedly fail so the agent can steer the model off a
// retry loop instead of burning the whole iteration budget. key = "name|args";
// value = consecutive failure count.
class FailStreak {
public:
    // Fold a batch of calls into the streak. A fully-successful batch clears
    // all streaks; otherwise each call in a failing batch bumps its own
    // streak. Returns the worst (highest) streak seen.
    int update(const json& calls, bool all_ok);

    void clear() { streak_.clear(); }

private:
    std::map<std::string, int> streak_;
};

// The model is stuck retrying a tool that keeps failing. Inject steering
// guidance, record it, and set `final_reply`. Returns true (caller breaks).
bool inject_tool_recovery_steer(std::vector<Message>& history,
                                const AgentHooks& hooks, ConversationLog& log,
                                std::string& final_reply);

} // namespace agent

#endif // AGENT_TOOL_RECOVERY_H
