#include "tui/action_registry.h"

namespace tui {

void ActionRegistry::register_action(
    const std::string& action,
    std::function<void(const std::string&)> handler) {
    if (handlers_.count(action)) return;
    handlers_[action] = std::move(handler);
}

bool ActionRegistry::has(const std::string& action) const noexcept {
    return handlers_.count(action) != 0;
}

bool ActionRegistry::dispatch(const std::string& action,
                              const std::string& arg) const {
    auto it = handlers_.find(action);
    if (it == handlers_.end()) return false;
    it->second(arg);
    return true;
}

} // namespace tui
