#ifndef AMBER_TUI_SLASH_DISPATCHER_H
#define AMBER_TUI_SLASH_DISPATCHER_H

#include <functional>
#include <string>
#include <vector>

namespace tui {
class Tui;

class SlashDispatcher {
public:
    explicit SlashDispatcher(Tui& tui);
    bool handle_slash(const std::string& line);
    void register_builtin_actions();
    void build_commands();

private:
    Tui& tui_;
    void register_action(const std::string& action, std::function<void(const std::string&)> handler);
};

} // namespace tui

#endif
