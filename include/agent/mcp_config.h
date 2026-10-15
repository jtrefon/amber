
#ifndef AGENT_MCP_CONFIG_H
#define AGENT_MCP_CONFIG_H

#include <map>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

#include "agent/mcp_client.h"
#include "agent/process.h"

namespace agent {

// One declared MCP server (from a config file or /mcp add).
struct McpServerConfig {
    std::string name;              // kebab-case; the namespacing root
    std::string type;              // "stdio" | "http"
    std::string command;           // stdio: executable (no shell)
    std::vector<std::string> args; // stdio: space-separated in the file
    std::string cwd;               // stdio: empty = workspace root
    std::string url;               // http
    std::string auth_token;        // http: static bearer token (never logged)
    bool enabled = true;
    bool auto_connect = false;
    bool trusted = false;
    int timeout_s = 60;
    std::string error; // validation error ("" when valid)
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
    explicit ServerManager(std::map<std::string, McpServerConfig> servers = {},
                           const CancellationToken* cancel_token = nullptr);

    // Connect every enabled && auto_connect server; failures are recorded per
    // server and never thrown.
    void connect_all();

    std::string connect(const std::string& name);
    void disconnect(const std::string& name);
    std::string refresh(const std::string& name) const;

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

    // Shared lease: tool adapters hold it across a disconnect so an
    // in-flight call never touches a destroyed client.
    std::shared_ptr<MCPClient> client(const std::string& name) const;

    // The exact adapter names registered for a server (see
    // register_server_tools). Unwinding removes these and nothing else — the
    // record is what replaced the name-prefix sweep, which could reach tools
    // another owner had contributed.
    void note_registered_tools(const std::string& server, std::vector<std::string> names);
    std::vector<std::string> registered_tools(const std::string& server) const;
    void forget_registered_tools(const std::string& server);

    // Disconnect everything (session end).
    void shutdown_all();

private:
    // Guards configs_/clients_/connect_gen_/registered_: the UI thread mutates
    // them while worker threads may take client leases (client()) or read
    // status.
    mutable std::mutex mtx_;
    std::map<std::string, McpServerConfig> configs_;
    std::map<std::string, std::shared_ptr<MCPClient>> clients_;
    // Per-server connect generation: a stale handshake (started before a
    // disable/remove/re-connect) must not install its client afterwards.
    std::map<std::string, uint64_t> connect_gen_;
    // What the host registered per server, in registration order.
    std::map<std::string, std::vector<std::string>> registered_;
    const CancellationToken* cancel_token_ = nullptr;
};

} // namespace agent

#endif // AGENT_MCP_CONFIG_H
