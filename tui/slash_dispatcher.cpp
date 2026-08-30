#include "slash_dispatcher.h"
#include "tui.h"

namespace tui {

SlashDispatcher::SlashDispatcher(Tui& tui) : tui_(tui) {}

bool SlashDispatcher::handle_slash(const std::string& line) {
    return tui_.handle_slash(line);
}

void SlashDispatcher::register_builtin_actions() {
    tui_.register_builtin_actions();
}

void SlashDispatcher::build_commands() {
    tui_.build_commands();
}

void SlashDispatcher::register_action(const std::string& action, std::function<void(const std::string&)> handler) {
    tui_.register_action(action, std::move(handler));
}

} // namespace tui
