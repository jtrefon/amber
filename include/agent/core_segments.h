#ifndef AGENT_CORE_SEGMENTS_H
#define AGENT_CORE_SEGMENTS_H

// Amber's own status-bar segments, as capability declarations.
//
// They go through the same declare-and-install path a plugin's contributions
// do instead of writing the registry directly, which is the point: the path
// that ships is the path the app itself exercises, so a capability whose
// install or unwind is broken cannot hide behind "no plugin uses it yet".
// The runtime installs them under a reserved "core" owner (install_core_ui).
//
// The order and drop priorities below reproduce the bar as it was hardcoded.

#include "agent/plugin_capability.h"

#include <memory>
#include <vector>

namespace agent {

// One capability per built-in segment, in no particular order: the registry
// sorts by priority once they are installed.
std::vector<std::unique_ptr<Capability>> core_status_capabilities();

} // namespace agent

#endif // AGENT_CORE_SEGMENTS_H
