#ifndef TUI_TASK_RUNTIME_H
#define TUI_TASK_RUNTIME_H

#include "agent/task_inbox.h"

namespace agent {
class EventBus;
namespace plugins {
class WatchPlugin;
} // namespace plugins
} // namespace agent

namespace tui::detail {

// The TUI's background-work state: the inventory, its courier, the resolved
// watch producer and the watch subscription. One owned component keeps the Tui
// class inside the size cap.
struct TaskRuntime {
    TaskRuntime(agent::EventBus& bus, agent::TaskService& tasks) : inbox(bus, tasks) {}
    agent::TaskInbox inbox;
    agent::plugins::WatchPlugin* watch_plugin = nullptr;
    agent::Subscription watch_sub;
};

} // namespace tui::detail

#endif // TUI_TASK_RUNTIME_H
