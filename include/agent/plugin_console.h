#ifndef AGENT_PLUGIN_CONSOLE_H
#define AGENT_PLUGIN_CONSOLE_H

// The registry console: what is loaded, what is on, and what each plugin
// contributes.
//
// It is core UI built on the same public panel API a plugin uses (it is
// registered like any other panel), and it is always available: a plugin must
// never be able to hide the view that says whether plugins are working.
//
// The formatting lives here rather than in the TUI so the console panel and
// the `/get plugin` command report the same thing from one implementation.

#include "agent/extensions.h"
#include "agent/plugin_runtime.h"

#include <string>
#include <vector>

namespace agent {

// One line per plugin (fixed column order: id, tier, state, version,
// contributions), then one line per registered panel. Pure: reads the runtime,
// writes nothing.
std::vector<std::string> plugin_console_lines(const PluginRuntime& runtime);

// Register the console panel. Called by the runtime itself.
void register_console_panel(PanelRegistry& panels, const PluginRuntime& runtime);

} // namespace agent

#endif // AGENT_PLUGIN_CONSOLE_H
