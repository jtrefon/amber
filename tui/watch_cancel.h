#ifndef AMBER_TUI_WATCH_CANCEL_H
#define AMBER_TUI_WATCH_CANCEL_H

#include <string>

namespace agent {
class PluginRuntime;
} // namespace agent

namespace tui {

// Cancel and abandon every live watch owned by `owner_id`. Returns how many
// non-terminal watches were asked to stop. Abandoning is deliberate: a watch
// the user stopped must not wake the window with a completion turn.
int cancel_owned_watches(agent::PluginRuntime& runtime, const std::string& owner_id);

} // namespace tui

#endif // AMBER_TUI_WATCH_CANCEL_H
