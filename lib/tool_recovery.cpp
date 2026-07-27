// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/tool_recovery.h"
#include "agent/agent_helpers.h"

namespace agent {

int FailStreak::update(const json& calls, bool all_ok) {
    if (all_ok) { streak_.clear(); return 0; }
    if (calls.is_null() || !calls.is_array()) return 0;
    int worst = 0;
    std::string fp = fingerprint_tool_calls(calls);
    if (fp.empty()) return 0;
    // Each call in a failing batch gets its own streak under the same key so
    // that repeated identical call-sets escalate quickly.
    worst = std::max(worst, ++streak_[fp]);
    return worst;
}

void inject_tool_recovery_steer(std::vector<Message>& history,
                                const AgentHooks& hooks, ConversationLog& log) {
    Message steer;
    steer.role = "user";
    steer.content =
        "Some tool calls are failing repeatedly (see the ERROR "
        "messages above). Do not retry a failing call with the same "
        "arguments. Either provide corrected arguments, switch to a "
        "different tool, or stop and give your best answer now.";
    history.push_back(std::move(steer));
    if (hooks.on_status) hooks.on_status("tool recovery: injected steer");
    log.event("tool_recovery", {{"action", "steer"}});
}

} // namespace agent
