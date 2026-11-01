#ifndef AGENT_EXIT_READY_H
#define AGENT_EXIT_READY_H

namespace agent {

// Announce that this process reached the end of its own quit path: the terminal
// is restored, the sessions and the workspace are saved, and main is about to
// return. A test harness waits for this file instead of guessing how long a save
// takes on the machine it is running on; a killed child never runs its atexit
// handlers, so a timeout that is too short becomes a silent coverage hole rather
// than a failure.
//
// The path comes from AMBER_EXIT_READY_FILE. It is set only by a harness, and
// without it this does nothing. Returns whether the announcement was written.
bool announce_exit_ready();

} // namespace agent

#endif // AGENT_EXIT_READY_H
