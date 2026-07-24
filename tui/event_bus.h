// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AMBER_TUI_EVENT_BUS_H
#define AMBER_TUI_EVENT_BUS_H

#include <agent/agent.h>  // RunState, Stats, ToolResult, json, Approval, CompressionResult

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace tui {

// ---------------------------------------------------------------------------
// Event types — each is a lightweight struct with only the fields it needs.
// ---------------------------------------------------------------------------

struct Event {
    virtual ~Event() = default;
};

struct TokenReceived         : Event { std::string text; TokenReceived(std::string t) : text(std::move(t)) {} };
struct ReasoningReceived     : Event { std::string text; ReasoningReceived(std::string t) : text(std::move(t)) {} };
struct StateChanged          : Event { agent::RunState state; explicit StateChanged(agent::RunState s) : state(s) {} };
struct ToolCallDispatched    : Event { std::string name; agent::json args; ToolCallDispatched(std::string n, agent::json a) : name(std::move(n)), args(std::move(a)) {} };
struct ToolResultReceived    : Event { std::string name; agent::ToolResult result; ToolResultReceived(std::string n, agent::ToolResult r) : name(std::move(n)), result(std::move(r)) {} };
struct StatusMessage         : Event { std::string text; StatusMessage(std::string t) : text(std::move(t)) {} };
struct StatsUpdated          : Event { agent::Stats stats; explicit StatsUpdated(agent::Stats s) : stats(s) {} };
struct AssistantDone         : Event { std::string text; AssistantDone(std::string t) : text(std::move(t)) {} };
struct ErrorOccurred         : Event { std::string message; ErrorOccurred(std::string m) : message(std::move(m)) {} };
struct PromptSubmitted       : Event { std::string text; PromptSubmitted(std::string t) : text(std::move(t)) {} };
struct CancelRequested       : Event {};
struct CompressionRequested  : Event {};
struct CompressionDone       : Event { agent::CompressionResult result; explicit CompressionDone(agent::CompressionResult r) : result(r) {} };
struct SessionSaved          : Event { std::string id; SessionSaved(std::string i) : id(std::move(i)) {} };
struct QuitRequested         : Event {};

// ---------------------------------------------------------------------------
// EventBus — typed publish-subscribe.
// ---------------------------------------------------------------------------

class EventBus : public std::enable_shared_from_this<EventBus> {
public:
    EventBus() = default;
    ~EventBus() = default;
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;

    // Subscribe to an event type. Handler is invoked on dispatch() when
    // called from the main thread, or queued from other threads.
    template<typename E, typename F>
    void on(F&& handler) {
        auto slot = std::make_shared<Slot<E>>(std::forward<F>(handler));
        std::lock_guard<std::mutex> lock(slots_mtx_);
        slots_[std::type_index(typeid(E))].push_back(std::move(slot));
    }

    // Emit an event. On the main thread handlers fire immediately.
    // From other threads the event is queued and delivered on the next
    // dispatch() call.
    template<typename E>
    void emit(E&& event) {
        if (on_main_thread()) {
            deliver(std::forward<E>(event));
        } else {
            auto ptr = std::make_shared<E>(std::forward<E>(event));
            std::lock_guard<std::mutex> lock(pending_mtx_);
            pending_.push({std::type_index(typeid(E)), ptr});
        }
    }

    // Dispatch all queued cross-thread events. Called once per frame
    // from the main thread.
    void dispatch() {
        std::queue<Pending> q;
        {
            std::lock_guard<std::mutex> lock(pending_mtx_);
            q.swap(pending_);
        }
        while (!q.empty()) {
            auto& p = q.front();
            auto it = slots_.find(p.type);
            if (it != slots_.end()) {
                for (auto& slot : it->second) {
                    if (p.event) {
                        slot->call(p.event.get());
                    }
                }
            }
            q.pop();
        }
    }

    // Helpers for hook wiring in AgentService.
    void emit_token(const std::string& t) { emit(TokenReceived{t}); }
    void emit_reasoning(const std::string& t) { emit(ReasoningReceived{t}); }
    void emit_state(agent::RunState s) { emit(StateChanged{s}); }
    void emit_stats(const agent::Stats& s) { emit(StatsUpdated{s}); }
    void emit_status(const std::string& s) { emit(StatusMessage{s}); }
    void emit_tool_call(const std::string& n, const agent::json& a) {
        emit(ToolCallDispatched{n, a});
    }
    void emit_tool_result(const std::string& n, const agent::ToolResult& r) {
        emit(ToolResultReceived{n, r});
    }
    void emit_assistant(const std::string& t) { emit(AssistantDone{t}); }
    void emit_error(const std::string& m) { emit(ErrorOccurred{m}); }

private:
    // Base for type-erased slot storage
    struct SlotBase {
        virtual ~SlotBase() = default;
        virtual void call(const Event* e) = 0;
    };

    template<typename E>
    struct Slot : SlotBase {
        std::function<void(const E&)> handler;
        explicit Slot(std::function<void(const E&)>&& h) : handler(std::move(h)) {}
        void call(const Event* e) override { handler(*static_cast<const E*>(e)); }
    };

    struct Pending {
        std::type_index type;
        std::shared_ptr<Event> event;
    };

    static bool on_main_thread() {
        static const auto main_id = std::this_thread::get_id();
        return std::this_thread::get_id() == main_id;
    }

    template<typename E>
    void deliver(E&& event) {
        auto it = slots_.find(std::type_index(typeid(E)));
        if (it != slots_.end()) {
            for (auto& slot : it->second) {
                slot->call(&event);
            }
        }
    }

    std::unordered_map<std::type_index, std::vector<std::shared_ptr<SlotBase>>> slots_;
    std::mutex slots_mtx_;
    std::queue<Pending> pending_;
    std::mutex pending_mtx_;
};

} // namespace tui

#endif // AMBER_TUI_EVENT_BUS_H
