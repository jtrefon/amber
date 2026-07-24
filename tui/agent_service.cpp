// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "tui/agent_service.h"

namespace tui {

AgentService::AgentService(agent::Config cfg, agent::ToolRegistry& reg,
                           std::shared_ptr<EventBus> bus)
    : bus_(std::move(bus)), cfg_(std::move(cfg)), reg_(reg) {
    bus_->on<PromptSubmitted>([this](const PromptSubmitted& e) { on_prompt(e); });
    bus_->on<CancelRequested>([this](const CancelRequested&) { on_cancel(); });

    thread_ = std::thread([this] { worker_loop(); });
}

AgentService::~AgentService() {
    stop_requested_.store(true);
    if (thread_.joinable()) thread_.join();
}

void AgentService::start(const std::string& prompt) {
    bus_->emit(PromptSubmitted{prompt});
}

void AgentService::cancel() {
    cfg_.cancel_token.request();
}

void AgentService::on_prompt(const PromptSubmitted& e) {
    if (agent_busy_.load()) return;
    agent_busy_.store(true);

    agent::ToolRegistry reg;
    agent::Agent agent(cfg_, reg,
        /*hooks*/{}, {}, {}, {}, {});

    // Set up hooks that translate to events
    agent::AgentHooks hooks;
    hooks.on_token = [this](const std::string& t) { bus_->emit_token(t); };
    hooks.on_reasoning = [this](const std::string& t) { bus_->emit_reasoning(t); };
    hooks.on_state = [this](agent::RunState s) { bus_->emit_state(s); };
    hooks.on_status = [this](const std::string& s) { bus_->emit_status(s); };
    hooks.on_tool_call = [this](const std::string& n, const agent::json& a) {
        bus_->emit_tool_call(n, a);
    };
    hooks.on_tool_result = [this](const std::string& n, const agent::ToolResult& r) {
        bus_->emit_tool_result(n, r);
    };
    hooks.on_stats = [this](const agent::Stats& s) { bus_->emit_stats(s); };
    hooks.on_assistant = [this](const std::string& t) { bus_->emit_assistant(t); };

    agent.set_hooks(std::move(hooks));
    agent.run(e.text);

    agent_busy_.store(false);
}

void AgentService::on_cancel() { cancel(); }

void AgentService::worker_loop() {
    // Wait for prompts — currently on_prompt is called from the bus
    // dispatch on the main thread. The Agent::run() is synchronous
    // within that call. For a fully non-blocking design, the worker
    // thread would pick up prompts from its own queue.
}

} // namespace tui
