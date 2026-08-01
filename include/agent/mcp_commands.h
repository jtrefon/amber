
#ifndef AGENT_MCP_COMMANDS_H
#define AGENT_MCP_COMMANDS_H

#include <string>
#include <vector>

#include "agent/mcp_config.h"
#include "agent/mcp_tools.h"
#include "agent/registry.h"

namespace agent {

// Curation surface shared by the TUI (/mcp, /prompt) and the CLI. Each
// function returns human-readable status lines or an error string, so hosts
// stay thin.

// "name · type · state · tools · prompts · error" lines for /mcp list.
std::vector<std::string> mcp_list_lines(const ServerManager& mgr);

// Capabilities + tool/resource/prompt tables for /mcp show <server>.
std::vector<std::string> mcp_show_lines(const ServerManager& mgr,
                                        const std::string& server,
                                        std::string& error);

// Connect (or reconnect) and register the server's tool adapters.
std::string mcp_connect(ServerManager& mgr, ToolRegistry& reg,
                        const std::string& server);

// Disconnect and unregister the server's tool adapters.
void mcp_disconnect(ServerManager& mgr, ToolRegistry& reg,
                    const std::string& server);

// Re-discover and re-register the adapters.
std::string mcp_refresh(ServerManager& mgr, ToolRegistry& reg,
                        const std::string& server);

// enable/disable (disable disconnects + unregisters); trust on|off.
std::string mcp_enable(ServerManager& mgr, ToolRegistry& reg,
                       const std::string& server, bool on);
std::string mcp_trust(ServerManager& mgr, const std::string& server, bool on);

// /prompt <server> <name> [k=v ...]: validate required args client-side, get
// the template, and return it in `out_text`. Error string on failure.
std::string mcp_prompt(ServerManager& mgr, const std::string& server,
                       const std::string& name, const json& arguments,
                       std::string& out_text);

} // namespace agent

#endif // AGENT_MCP_COMMANDS_H
