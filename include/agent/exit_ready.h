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
// AMBER_EXIT_READY turns this on and names nothing: the file is
// `<cache dir>/exit-ready-<pid>`, derived by the process itself. A caller-chosen
// path would make the last thing a process does a write anywhere the user can
// reach, which is not a capability a harness needs. The cache root rather than
// the project state dir because a session can switch project (a worktree, a
// project window), and this is about the process, not the project.
//
// Returns whether the announcement was written.
bool announce_exit_ready();

} // namespace agent

#endif // AGENT_EXIT_READY_H
