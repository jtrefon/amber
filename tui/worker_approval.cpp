#include "tui/worker_approval.h"

#include "tui/agent_event.h"
#include "tui/event_router.h"
#include "tui/window.h"
#include "tui/window_manager.h"

#include <future>
#include <memory>
#include <utility>

namespace tui {

namespace {

// Same contract as the foreground hooks: queue on the UI thread unless the
// router is shutting down, in which case the caller answers fail-closed rather
// than blocking on a promise nobody will keep.
bool push_event(EventRouter& router, AgentEvent ev, std::size_t window_id) {
    std::scoped_lock lk(router.mutex());
    if (router.shutting_down())
        return false;
    ev.window_id = window_id;
    router.queue().push(std::move(ev));
    return true;
}

std::string worker_text(const WorkerTarget& target, const std::string& body) {
    return "worker [" + target.owner_title + "]: " + body;
}

} // namespace

agent::AgentHooks worker_hooks(EventRouter& router, WindowManager& windows,
                               const std::string& owner_id) {
    agent::AgentHooks hooks;
    hooks.on_approval = [&router, &windows,
                         owner_id](const std::string&, const agent::json&,
                                   const std::string& summary) -> agent::Approval {
        const WorkerTarget target = resolve_worker_target(windows, owner_id);
        if (!target.ok)
            return agent::Approval::Deny;
        auto promise = std::make_shared<std::promise<agent::Approval>>();
        auto future = promise->get_future();
        AgentEvent ev;
        ev.type = AgentEvent::Approval;
        ev.owner_id = owner_id;
        ev.text = worker_text(target, summary);
        ev.approval_promise = promise;
        if (!push_event(router, std::move(ev), target.window_id))
            return agent::Approval::Deny;
        return future.get();
    };
    hooks.on_api_key = [&router, &windows, owner_id](const std::string& reason) -> std::string {
        const WorkerTarget target = resolve_worker_target(windows, owner_id);
        if (!target.ok)
            return {};
        auto promise = std::make_shared<std::promise<std::string>>();
        auto future = promise->get_future();
        AgentEvent ev;
        ev.type = AgentEvent::ApiKey;
        ev.owner_id = owner_id;
        ev.text = worker_text(target, reason);
        ev.api_key_promise = promise;
        if (!push_event(router, std::move(ev), target.window_id))
            return {};
        return future.get();
    };
    return hooks;
}

} // namespace tui
