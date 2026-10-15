#ifndef AGENT_PLUGINS_MCP_MCP_PLUGIN_H
#define AGENT_PLUGINS_MCP_MCP_PLUGIN_H

#include "agent/extensions.h"
#include "agent/mcp_config.h"
#include "agent/plugin_core.h"

#include <memory>
#include <string>
#include <vector>

namespace agent {
class PluginRuntime;
class ToolRegistry;
class UiServices;
} // namespace agent

namespace agent::plugins {

// The MCP client as a plugin. A server's tool set is discovered, never declared
// (the server advertises it after it has been spawned), so this plugin uses the
// tool-source seam: it holds the ServerManager and registers what its servers
// discover through the runtime's owner-scoped port. Its other surfaces — the
// status segment, the prompt block and the /mcp, /prompt and /get mcp command
// namespaces — are ordinary contributions, which is what makes
// `/set plugin mcp off` a complete footprint switch: no servers spawned, no
// tools, no prose describing them.
class McpPlugin : public IPlugin {
public:
    std::string id() const override { return "mcp"; }
    std::string version() const override { return "1.0.0"; }
    std::string name() const override { return "MCP client"; }
    std::string description() const override {
        return "Model Context Protocol servers: the tools, resources and prompts they expose.";
    }
    std::string category() const override { return "integration"; }

    bool initialize(const PluginContext&) override { return true; }
    void shutdown() override;

    std::vector<std::unique_ptr<Capability>> capabilities() override;

    // Host-facing API: the CLI's --mcp* surfaces and tests reach the same state
    // the slash commands do. While the plugin is inactive the manager does not
    // exist, so every call is a no-op or reports "not active".
    std::string connect(const std::string& server);
    void disconnect(const std::string& server);
    std::string refresh(const std::string& server);
    std::string set_enabled(const std::string& server, bool on);
    std::string set_trusted(const std::string& server, bool on);
    std::vector<std::string> list_lines() const;
    std::vector<std::string> show_lines(const std::string& server, std::string& error) const;
    std::string prompt_text(const std::string& server, const std::string& name,
                            const json& arguments, std::string& out_text) const;

    // Null while the plugin is inactive.
    const ServerManager* manager() const noexcept { return mgr_.get(); }

private:
    // The tool-source half: start brings the manager up and registers what the
    // enabled servers discover; stop tears all of it down (the runtime clears
    // the port right after).
    void start(PluginServices& services, ToolPort& port);
    void stop();

    // The command surfaces. The /mcp verbs are one small handler each, so the
    // spec builder is wiring, not a hundred-line function.
    CommandSpec build_mcp_spec();
    CommandSpec build_prompt_spec();
    CommandSpec build_get_spec();

    std::string servers_text() const;
    std::string root_handler(const std::string& rest) const;
    std::string show_handler(const std::string& arg) const;
    std::string connect_handler(const std::string& arg);
    std::string disconnect_handler(const std::string& arg);
    std::string refresh_handler(const std::string& arg);
    std::string prompts_handler(const std::string& arg) const;
    std::string set_enabled_handler(const std::string& arg, bool on);
    std::string trust_handler(const std::string& arg);

    std::unique_ptr<ServerManager> mgr_;
    ToolPort* port_ = nullptr;               // borrowed; valid while active
    const ToolRegistry* registry_ = nullptr; // borrowed; read for name collisions
    UiServices* ui_ = nullptr;               // borrowed; /prompt fills the input line
    // The plugin's own token, so stop() aborts an in-flight handshake without
    // touching the host's token (which cancels an agent run, not a server).
    CancellationToken cancel_;
};

// The instance a host reaches through the runtime (PluginRuntime::find is the
// sanctioned host accessor). Null when the plugin is off or absent.
McpPlugin* mcp_plugin_from(PluginRuntime& runtime);

} // namespace agent::plugins

#endif // AGENT_PLUGINS_MCP_MCP_PLUGIN_H
