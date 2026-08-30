#include "event_router.h"

#include <utility>

namespace tui {

void EventRouter::push(AgentEvent ev) {
    std::scoped_lock lk(mtx_);
    queue_.push(std::move(ev));
}

std::vector<AgentEvent> EventRouter::pop_all() {
    std::vector<AgentEvent> out;
    std::scoped_lock lk(mtx_);
    while (!queue_.empty()) {
        out.push_back(std::move(queue_.front()));
        queue_.pop();
    }
    return out;
}

bool EventRouter::empty() const {
    std::scoped_lock lk(mtx_);
    return queue_.empty();
}

void EventRouter::clear() {
    std::scoped_lock lk(mtx_);
    std::queue<AgentEvent> empty;
    std::swap(queue_, empty);
}

void EventRouter::join_thread() {
    if (thread_.joinable()) thread_.join();
}

void EventRouter::shutdown_queues(std::queue<AgentEvent>& pending_approvals) {
    std::scoped_lock lk(mtx_);
    shutting_down_ = true;
    deny_all_pending_approvals(queue_);
    deny_all_pending_approvals(pending_approvals);
}

agent::AgentHooks EventRouter::make_hooks(size_t window_id) {
    agent::AgentHooks hooks;
    auto push_event = [this, window_id](AgentEvent ev) {
        if (cancel_.load()) return;
        ev.window_id = window_id;
        std::scoped_lock lk(mtx_);
        queue_.push(std::move(ev));
    };

    hooks.on_reasoning = [push_event](const std::string& d) {
        AgentEvent ev;
        ev.type = AgentEvent::Reasoning;
        ev.text = d;
        push_event(std::move(ev));
    };
    hooks.on_token = [push_event](const std::string& d) {
        AgentEvent ev;
        ev.type = AgentEvent::Token;
        ev.text = d;
        push_event(std::move(ev));
    };
    hooks.on_state = [push_event](agent::RunState s) {
        AgentEvent ev;
        ev.type = AgentEvent::StateChange;
        ev.state = s;
        push_event(std::move(ev));
    };
    hooks.on_stats = [push_event](const agent::Stats& s) {
        AgentEvent ev;
        ev.type = AgentEvent::Stats;
        ev.stats = s;
        push_event(std::move(ev));
    };
    hooks.on_status = [push_event](const std::string& s) {
        AgentEvent ev;
        ev.type = AgentEvent::Status;
        ev.text = s;
        push_event(std::move(ev));
    };
    hooks.on_tool_call = [push_event](const std::string& n, const agent::json& a) {
        AgentEvent ev;
        ev.type = AgentEvent::ToolCall;
        ev.tool_name = n;
        ev.tool_args = a;
        push_event(std::move(ev));
    };
    hooks.on_tool_result = [push_event](const std::string& n,
                                        const agent::ToolResult& r,
                                        const agent::json& a) {
        AgentEvent ev;
        ev.type = AgentEvent::ToolResult;
        ev.tool_name = n;
        ev.tool_args = a;
        ev.tool_result = r;
        push_event(std::move(ev));
    };
    hooks.on_assistant = [push_event](const std::string& s) {
        AgentEvent ev;
        ev.type = AgentEvent::Assistant;
        ev.text = s;
        push_event(std::move(ev));
    };
    hooks.on_approval = [this, window_id](const std::string& name,
                                          const agent::json& args,
                                          const std::string& summary) -> agent::Approval {
        if (cancel_.load()) return agent::Approval::Deny;
        auto p = std::make_shared<std::promise<agent::Approval>>();
        auto f = p->get_future();
        AgentEvent ev;
        ev.type = AgentEvent::Approval;
        ev.text = summary;
        ev.approval_promise = p;
        {
            std::scoped_lock lk(mtx_);
            if (shutting_down_) return agent::Approval::Deny;
            ev.window_id = window_id;
            queue_.push(std::move(ev));
        }
        (void)name;
        (void)args;
        return f.get();
    };

    return hooks;
}

} // namespace tui
