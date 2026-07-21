// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AGENT_PROCESS_H
#define AGENT_PROCESS_H

#include <string>
#include <sys/types.h>

namespace agent {

// Low-level POSIX process helpers shared by the shell tool and the background
// job service. A command is spawned via `/bin/sh -c` with stdout and stderr
// fused onto a single pipe, in its own process group, so the whole subtree can
// be reaped reliably with one kill().

// Spawn `command` in its own process group. On success returns the child pid
// and hands back the read end of the fused stdout/stderr pipe via `read_fd`
// (caller owns and must close it). On failure returns -1 with `err` set.
pid_t spawn_shell(const std::string& command, const std::string& cwd,
                  int& read_fd, std::string& err);

// Kill an entire process group spawned by spawn_shell (SIGKILL).
void kill_process_group(pid_t pid);

} // namespace agent

#endif // AGENT_PROCESS_H
