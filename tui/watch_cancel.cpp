#include "tui/watch_cancel.h"

#include "agent/plugin_runtime.h"
#include "plugins/watch/watch_plugin.h"

#include <vector>

namespace tui {

int cancel_owned_watches(agent::PluginRuntime& runtime, const std::string& owner_id) {
    auto* watch = dynamic_cast<agent::plugins::WatchPlugin*>(runtime.find("watch"));
    if (!watch)
        return 0;
    std::vector<std::string> running;
    for (const auto& record : watch->list_records())
        if (record.owner_id == owner_id && !agent::plugins::watch_state_terminal(record.state))
            running.push_back(record.id);
    if (running.empty())
        return 0;
    // Ack first: the worker's terminal write merges `delivered` and `abandoned`
    // from the record it replaces, so either flag closes the wake even if the
    // cancellation lands between the two calls. Abandoning then cancels the
    // worker, so the turn the user stopped cannot restart itself.
    for (const auto& id : running)
        watch->ack(id);
    watch->abandon_owner(owner_id);
    return static_cast<int>(running.size());
}

} // namespace tui
