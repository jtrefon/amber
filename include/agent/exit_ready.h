#ifndef AGENT_EXIT_READY_H
#define AGENT_EXIT_READY_H

#include <unistd.h>

namespace agent {

// The line a host writes after its quit path has finished, for a harness that
// waits on an event instead of guessing how long a save takes. It is short, it
// survives the CR/LF translation that follows endwin(), and it is written only
// when AMBER_EXIT_READY is set, so a normal run prints nothing.
inline constexpr char kExitReadyMarker[] = "[amber] exit-ready";

// Announce that this process reached the end of its own quit path: the terminal
// is restored, the sessions and the workspace are saved, and main is about to
// return. A killed child never runs its atexit handlers, so a timeout that is
// too short becomes a silent coverage hole rather than a failure; the marker is
// evidence that the saves happened, which a timeout can never be.
//
// The marker goes to the terminal rather than a file: the pty is already the
// channel the harness reads, so there is no path to derive, no state directory
// to agree on across a project switch, and nothing a caller could redirect.
//
// Returns whether the marker was written.
bool announce_exit_ready(int fd = STDOUT_FILENO);

} // namespace agent

#endif // AGENT_EXIT_READY_H
