
#ifndef AGENT_PLUGIN_H
#define AGENT_PLUGIN_H

#include "agent/registry.h"
#include "agent/tool.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace agent {

namespace plugin_internal {
class PluginTool;
}

// Parsed plugin manifest (docs/spec/plugins/README.md). `completion` is a
// completions.json subtree merged into the command tree on enable; `tools`
// are the tool definitions registered as plugin_<id>_<name>.
struct PluginManifest {
    std::string id;
    std::string name;
    std::string version;
    int protocol_version = 0;
    std::string author;
    std::string url;
    std::string license;
    std::string description;
    std::string main;
    json completion;
    json tools;
    json default_settings;
};

enum class PluginState : std::uint8_t { Disabled, Enabled, Incompatible };

struct PluginInfo {
    std::string id;
    std::string dir;
    std::string version;
    PluginState state = PluginState::Disabled;
    std::string error; // load / handshake failure detail
    json settings;
    PluginManifest manifest;
};

namespace plugin_internal {
class PluginTool;
}

class ToolPort;

// Discovers, spawns, and registers plugins. A plugin is a directory with a
// manifest.json and an executable speaking newline-delimited JSON-RPC over
// stdio (protocol 1). The harness never links plugin code.
class PluginManager {
public:
    PluginManager() = default;
    ~PluginManager(); // shuts down all live plugin processes

    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    // Scan plugin directories (given dirs when non-empty, else the standard
    // user/workspace/system locations) and load+validate every manifest.
    // Never throws; failures are recorded per plugin.
    void discover(const std::vector<std::string>& dirs = {});

    const std::vector<PluginInfo>& plugins() const { return plugins_; }
    const PluginInfo* find(const std::string& id) const;
    PluginInfo* find(const std::string& id);

    // Production lifecycle: register the plugin's tools through the
    // owner-scoped port. The process is spawned lazily on the first tool call,
    // so activation never blocks the host. Returns false and sets info.error
    // when the plugin is unknown or incompatible.
    bool enable(const std::string& id, ToolPort& port);
    // Unregister nothing here: the runtime closes the port, which removes
    // exactly the tools this plugin contributed.
    bool disable(const std::string& id);

    // Compatibility path for bare hosts and tests that own a raw registry.
    // It registers ownerless tools and unwinds by prefix; the runtime uses the
    // ToolPort overload above.
    bool enable(const std::string& id, ToolRegistry& reg);
    bool disable(const std::string& id, ToolRegistry& reg);

    // Persisted key/value settings (state.json in the user config dir).
    std::string get_setting(const std::string& id, const std::string& key) const;
    bool set_setting(const std::string& id, const std::string& key, const std::string& value);

    // Install a plugin from a local tar.gz path or an http(s) URL. Returns
    // "" on success or a human-readable error. Installed plugins are staged
    // into the user config dir and left disabled.
    std::string install(const std::string& source);
    std::string uninstall(const std::string& id);

private:
    friend class plugin_internal::PluginTool;

    // The default search roots, when discover() is given none.
    static std::vector<std::string> default_plugin_roots();
    // A directory under a root becomes a PluginInfo; false when it is state
    // rather than an install (no manifest, or an id already claimed).
    bool plugin_info_at(const std::string& dir, PluginInfo& out);

    struct Session {
        pid_t pid = -1;
        int in_fd = -1;
        int out_fd = -1;
        std::string in_buf;
        std::timed_mutex mtx;
        // Set before teardown waits for the call lock: an in-flight read or
        // write wakes on its next poll slice and returns, so disabling is
        // bounded by the slice rather than the 60 s call deadline.
        std::atomic<bool> cancelled{false};
        // Teardown could not take the lock inside its deadline and has sent
        // SIGKILL; the in-flight call reaps and closes the session on its way
        // out. Also set when a failed exchange leaves the stream desynced.
        std::atomic<bool> broken{false};
    };
    // Shared lease: callers hold the session alive across a concurrent
    // disable() (which erases and shuts it down).
    std::shared_ptr<Session> session(PluginInfo& info);
    // Remove and return the registered session, or null when none is live.
    std::shared_ptr<Session> take_session(const std::string& id);
    bool spawn_and_handshake(PluginInfo& info, Session& s);
    bool fail_handshake(PluginInfo& info, Session& s, const std::string& error);
    bool spawn_child(PluginInfo& info, Session& s);
    bool write_initialize(PluginInfo& info, Session& s);
    bool read_initialize_reply(PluginInfo& info, Session& s);
    bool read_line(Session& s, const std::string& phase, std::string& line, std::string& error);
    bool write_line(Session& s, const std::string& data, std::string& error);
    ToolResult read_call_reply(Session& s);
    ToolResult call_tool(const PluginInfo& info, const std::string& name, const json& args);
    // Cancel an in-flight call, then wait a bounded time for the session lock
    // before shutting the process down. Never blocks on the call deadline.
    void teardown_session(const std::shared_ptr<Session>& session);
    // Drop `session` from the map (if it is still the registered one) and shut
    // it down; called with the session lock held.
    void discard_session(const std::string& id, const std::shared_ptr<Session>& session);
    void shutdown_session(Session& s);

    static bool parse_manifest(const std::string& dir, PluginManifest& out, std::string& err);
    void load_state();
    void save_state();

    std::vector<PluginInfo> plugins_;
    std::map<std::string, std::shared_ptr<Session>> sessions_;
    std::mutex sessions_mtx_; // guards sessions_ and PluginInfo::state
    json state_;
};

// Render the "Plugins" system-prompt section from the registry's plugin_*
// tools (name + description per enabled plugin tool).
std::string plugin_tools_advertisement(const ToolRegistry& reg);

} // namespace agent

#endif // AGENT_PLUGIN_H
