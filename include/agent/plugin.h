
#ifndef AGENT_PLUGIN_H
#define AGENT_PLUGIN_H

#include "agent/registry.h"
#include "agent/tool.h"

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
    std::string error;  // load / handshake failure detail
    json settings;
    PluginManifest manifest;
};

// Discovers, spawns, and registers plugins. A plugin is a directory with a
// manifest.json and an executable speaking newline-delimited JSON-RPC over
// stdio (protocol 1). The harness never links plugin code.
class PluginManager {
public:
    PluginManager() = default;
    ~PluginManager();  // shuts down all live plugin processes

    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    // Scan plugin directories (given dirs when non-empty, else the standard
    // user/workspace/system locations) and load+validate every manifest.
    // Never throws; failures are recorded per plugin.
    void discover(const std::vector<std::string>& dirs = {});

    const std::vector<PluginInfo>& plugins() const { return plugins_; }
    const PluginInfo* find(const std::string& id) const;
    PluginInfo* find(const std::string& id);

    // Lifecycle: enable spawns the process and performs the protocol
    // handshake, then registers the plugin's tools in `reg`. Returns false
    // and sets info.error on failure.
    bool enable(const std::string& id, ToolRegistry& reg);
    bool disable(const std::string& id, ToolRegistry& reg);

    // Persisted key/value settings (state.json in the user config dir).
    std::string get_setting(const std::string& id, const std::string& key) const;
    bool set_setting(const std::string& id, const std::string& key,
                     const std::string& value);

    // Install a plugin from a local tar.gz path or an http(s) URL. Returns
    // "" on success or a human-readable error. Installed plugins are staged
    // into the user config dir and left disabled.
    std::string install(const std::string& source);
    std::string uninstall(const std::string& id);

private:
    friend class plugin_internal::PluginTool;

    struct Session {
        pid_t pid = -1;
        int in_fd = -1;
        int out_fd = -1;
        std::string in_buf;
        std::mutex mtx;
    };
    Session& session(PluginInfo& info);
    bool spawn_and_handshake(PluginInfo& info, Session& s);
    ToolResult call_tool(const PluginInfo& info, const std::string& name,
                         const json& args);

    static bool parse_manifest(const std::string& dir, PluginManifest& out,
                               std::string& err);
    void load_state();
    void save_state();
    void shutdown_session(Session& s);

    std::vector<PluginInfo> plugins_;
    std::map<std::string, std::unique_ptr<Session>> sessions_;
    json state_;
};

// Render the "Plugins" system-prompt section from the registry's plugin_*
// tools (name + description per enabled plugin tool).
std::string plugin_tools_advertisement(const ToolRegistry& reg);

} // namespace agent

#endif // AGENT_PLUGIN_H
