// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AGENT_SESSION_H
#define AGENT_SESSION_H

#include <string>
#include <vector>
#include "agent/llm.h"

namespace agent {

// A persisted conversation: the full message history plus light metadata. Saved
// as a single JSON file so sessions are greppable and portable.
struct Session {
    std::string id;              // stable file id (timestamp-based, unique)
    std::string title;           // human label (defaults to first user line)
    std::string model;           // model in use when created
    long long created_ms = 0;    // unix ms
    long long updated_ms = 0;    // unix ms, bumped on save
    std::vector<Message> messages;

    // Serialize/deserialize to the on-disk JSON shape.
    json to_json() const;
    static Session from_json(const json& j);

    // Derive a short title from the first user message (trimmed to n chars).
    void derive_title(size_t max_len = 40);
};

// A short listing entry for pickers, without loading the whole message history.
struct SessionMeta {
    std::string id;
    std::string title;
    std::string model;
    long long updated_ms = 0;
    int message_count = 0;
    size_t file_size = 0;      // JSON file size in bytes
};

// Serializable workspace state: which sessions are open and which is active.
struct WorkspaceState {
    struct WindowEntry {
        std::string session_id;
        std::string title;
        std::vector<std::string> prompt_history;
    };
    std::vector<WindowEntry> windows;
    size_t active = 0;
    json to_json() const;
    static WorkspaceState from_json(const json& j);
};

// File-backed session store. Persists one JSON file per session under a
// directory (default: $XDG_DATA_HOME/amber/sessions, else
// ~/.local/share/amber/sessions). All methods are best-effort and never
// throw; failures return false / empty.
class SessionStore {
public:
    // Uses the default XDG directory when `dir` is empty.
    explicit SessionStore(const std::string& dir = "");

    const std::string& dir() const { return dir_; }

    // Create the storage directory if needed. Returns false if it can't.
    bool ensure_dir() const;

    // Mint a new unique session id (timestamp + counter).
    static std::string new_id();

    // Persist a session (creates or overwrites its file). Bumps updated_ms.
    bool save(Session& s) const;

    // Load a session by id. Returns false if missing/corrupt.
    bool load(const std::string& id, Session& out) const;

    // Delete a session file. Returns false if it did not exist.
    bool remove(const std::string& id) const;

    // List saved sessions, newest-updated first.
    // Uses a persistent index file for fast listing without reading every file.
    // Call rebuild_index() after any session save/remove to keep it fresh.
    std::vector<SessionMeta> list() const;
    void rebuild_index() const;
    bool list_contains(const std::string& id) const;

    // Workspace state: persist / restore open windows and their sessions.
    bool save_workspace(const WorkspaceState& ws) const;
    WorkspaceState load_workspace() const;

private:
    std::string dir_;
    mutable std::vector<SessionMeta> list_cache_;
    mutable bool cache_valid_ = false;
    std::string path_for(const std::string& id) const;
    std::string index_path() const;
    std::string workspace_path() const;
};

} // namespace agent

#endif // AGENT_SESSION_H
