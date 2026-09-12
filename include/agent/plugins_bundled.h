#ifndef AGENT_PLUGINS_BUNDLED_H
#define AGENT_PLUGINS_BUNDLED_H

// The bundled plugin set: every compiled-in plugin amber ships, in one list.
// Keeping registration in a single place is what makes the shipped set
// enumerable - the runtime, the console, and the tracker all read from here.

#include "agent/plugin_core.h"

#include <memory>
#include <vector>

namespace agent {

std::vector<std::shared_ptr<IPlugin>> make_bundled_plugins();

} // namespace agent

#endif // AGENT_PLUGINS_BUNDLED_H
