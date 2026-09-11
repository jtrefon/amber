#ifndef AGENT_CORE_SEGMENTS_H
#define AGENT_CORE_SEGMENTS_H

// Amber's own status-bar segments, registered like any other contribution.
//
// They are ordinary registry entries with the host as their owner, which is the
// point: a core segment and a plugin segment are the same kind of thing, so the
// registry is exercised by the app itself rather than only by a plugin. The
// order and drop priorities below reproduce the bar as it was hardcoded.

#include "agent/extensions.h"

namespace agent {

// Register the built-in segments. Idempotent per registry: registering twice
// would duplicate the bar, so callers register once at startup.
void register_core_status_segments(StatusRegistry& registry);

} // namespace agent

#endif // AGENT_CORE_SEGMENTS_H
