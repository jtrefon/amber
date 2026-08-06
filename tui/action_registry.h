#pragma once

#include <functional>
#include <map>
#include <string>

namespace tui {

// Command-action dispatch table. Registration is IDEMPOTENT: feeds re-merge
// identical closures on every refresh, and re-registering an action while
// its own handler is executing would destroy the live lambda (the handler
// itself triggers the refresh) — a use-after-free. First registration
// wins; behavior is a pure function of the action key, so re-registration
// is a no-op anyway.
class ActionRegistry {
public:
    void register_action(const std::string& action,
                         std::function<void(const std::string&)> handler);
    bool has(const std::string& action) const noexcept;
    // Returns false when the action has no handler.
    bool dispatch(const std::string& action, const std::string& arg) const;

private:
    std::map<std::string, std::function<void(const std::string&)>>
        handlers_;
};

} // namespace tui
