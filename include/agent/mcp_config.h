
#ifndef AGENT_MCP_CONFIG_H
#define AGENT_MCP_CONFIG_H

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "agent/mcp_client.h"

namespace agent {

// One declared MCP server (from a config file or /mcp add).
struct McpServerConfig {
    std::string name;          // kebab-case; the namespacing root
    std::string type;          // "stdio" | "http"
    std::string command;       // stdio: executable (no shell)
    std::vector<std::string> args;  // stdio: space-separated in the file
    std::string cwd;           // stdio: empty = workspace root
    std::string url;           // http
    std::string auth_token;    // http: static bearer token (never logged)
    bool enabled = true;
    bool auto_connect = false;
    bool trusted = false;
    int timeout_s = 60;
    std::string error;         // validation error ("" when valid)
};

// Load all server configs: global (~/.config/amber/mcp/) first, then project
// (.amber/mcp/, wins on name collisions). Invalid entries carry `error` and
// are never fatal. AMBER_MCP_SERVERS="a,b" overrides `enabled` for the run.
std::map<std::string, McpServerConfig> load_mcp_servers();

// Persist/remove a server config in the project dir.
bool save_mcp_server(const McpServerConfig& cfg);
bool delete_mcp_server(const std::string& name);

// One session's live view of a server (UI snapshots).
struct McpServerStatus {
    std::string name;
    std::string type;
    bool enabled = true;
    bool trusted = false;
    bool connected = false;
    int tool_count = 0;
    int resource_count = 0;
    int prompt_count = 0;
    std::string error;
};

// Session-scoped owner of MCP clients. Connects on demand or at session start
// (enabled + auto_connect), tears everything down on exit. Mutated from the
// agent thread; the UI reads snapshot().
class ServerManager {
public:
    explicit ServerManager(std::map<std::string, McpServerConfig> servers = {});

    // Connect every enabled && auto_connect server; failures are recorded per
    // server and never thrown.
    void connect_all();

    std::string connect(const std::string& name);
    void disconnect(const std::string& name);
    std::string refresh(const std::string& name);

    // Runtime policy toggles, persisted to the project config immediately.
    std::string set_trusted(const std::string& name, bool trusted);
    std::string set_enabled(const std::string& name, bool enabled);

    // Config CRUD for /mcp add|delete.
    std::string add_server(McpServerConfig cfg);
    std::string remove_server(const std::string& name);

    std::vector<McpServerStatus> snapshot() const;
    bool has_server(const std::string& name) const;
    bool trusted(const std::string& name) const;
    bool enabled(const std::string& name) const;

    MCPClient* client(const std::string& name);
    const MCPClient* client(const std::string& name) const;

    // Disconnect everything (session end).
    void shutdown_all();

private:
    std::map<std::string, McpServerConfig> configs_;
    std::map<std::string, std::unique_ptr<MCPClient>> clients_;
};

} // namespace agent

#endif // AGENT_MCP_CONFIG_H
