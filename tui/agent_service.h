// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AMBER_TUI_AGENT_SERVICE_H
#define AMBER_TUI_AGENT_SERVICE_H

#include "tui/event_bus.h"

#include <agent.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

namespace tui {

// Owns the Agent and runs it in a dedicated thread. Communicates with the
// rest of the UI via EventBus events.
class AgentService {
public:
    AgentService(agent::Config cfg, agent::ToolRegistry& reg,
                 std::shared_ptr<EventBus> bus);
    ~AgentService();

    AgentService(const AgentService&) = delete;
    AgentService& operator=(const AgentService&) = delete;

    void start(const std::string& prompt);
    void cancel();
    bool busy() const { return agent_busy_.load(); }

private:
    std::shared_ptr<EventBus> bus_;
    agent::Config cfg_;
    agent::ToolRegistry& reg_;
    std::thread thread_;
    std::atomic<bool> agent_busy_{false};
    std::atomic<bool> stop_requested_{false};

    void worker_loop();
    void on_prompt(const PromptSubmitted& e);
    void on_cancel();
};

} // namespace tui

#endif // AMBER_TUI_AGENT_SERVICE_H
