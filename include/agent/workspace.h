// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AGENT_WORKSPACE_H
#define AGENT_WORKSPACE_H

#include <string>

namespace agent {

// Path confinement for filesystem-touching tools.
//
// Tools may only read or write inside a "workspace root". By default the root
// is the process working directory at first use, but it can be overridden with
// the AMBER_WORKSPACE environment variable or set explicitly for tests.
//
// This is a defense-in-depth measure: the model driving the agent should not be
// able to escape the workspace via absolute paths (e.g. "/etc/passwd") or
// traversal ("../../../etc/shadow").
class Workspace {
public:
    // The active workspace root as an absolute, normalized path.
    static std::string root();

    // Override the root (used by tests and by the harness at startup).
    static void set_root(const std::string& path);

    // Resolve `path` (absolute or relative to the root) and verify it stays
    // within the root. On success `resolved` is the absolute normalized path
    // and the function returns true. On violation it returns false and fills
    // `error` with a message suitable for returning to the model.
    static bool confine(const std::string& path, std::string& resolved,
                        std::string& error);
};

} // namespace agent

#endif // AGENT_WORKSPACE_H
