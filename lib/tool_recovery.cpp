// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/tool_recovery.h"
#include "agent/agent_helpers.h"

namespace agent {

int FailStreak::update(const json& calls, bool all_ok) {
    if (all_ok) { streak_.clear(); return 0; }
    int worst = 0;
    for (const auto& call : calls) {
        if (!call.is_object()) continue;
        std::string fn, id;
        json args;
        bool ok = true;
        parse_tool_call(call, id, fn, args, ok);
        std::string key = fn + "|" + args.dump();
        worst = std::max(worst, ++streak_[key]);
    }
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
