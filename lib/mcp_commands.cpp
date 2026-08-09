
#include "agent/mcp_commands.h"

#include <algorithm>

namespace agent {

namespace {

const char* state_name(const McpServerStatus& st) {
    if (!st.enabled) return "disabled";
    if (st.connected) return "connected";
    return "disconnected";
}

} // namespace

std::vector<std::string> mcp_list_lines(const ServerManager& mgr) {
    std::vector<std::string> lines;
    for (const auto& st : mgr.snapshot()) {
        std::string line = st.name + " \u00b7 " + st.type + " \u00b7 " +
                           state_name(st) + " \u00b7 " +
                           std::to_string(st.tool_count) + " tools \u00b7 " +
                           std::to_string(st.prompt_count) + " prompts";
        if (!st.error.empty()) line += " \u00b7 " + st.error;
        lines.push_back(std::move(line));
    }
    return lines;
}

std::vector<std::string> mcp_show_lines(const ServerManager& mgr,
                                        const std::string& server,
                                        std::string& error) {
    std::vector<std::string> lines;
    const MCPClient* client = mgr.client(server);
    if (!client) {
        error = "server '" + server + "' not connected";
        return lines;
    }
    lines.push_back("server: " + server + " (" + client->server_info().name +
                    " " + client->server_info().version + ")");
    lines.push_back("tools: " + std::to_string(client->tools().size()));
    for (const auto& t : client->tools())
        lines.push_back("  " + agent::mcp_adapter_name(server, t.name) +
                        " \u2190 " + t.name);
    lines.push_back("resources: " + std::to_string(client->resources().size()));
    for (const auto& r : client->resources())
        lines.push_back("  " + r.uri + " \u00b7 " + r.name);
    lines.push_back("prompts: " + std::to_string(client->prompts().size()));
    for (const auto& p : client->prompts())
        lines.push_back("  " + p.name + " \u00b7 " + p.description);
    return lines;
}

std::string mcp_connect(ServerManager& mgr, ToolRegistry& reg,
                        const std::string& server) {
    // Unregister the old adapters BEFORE the reconnect: the new server's
    // tool set may no longer advertise tools that used to exist, and a stale
    // McpToolAdapter would keep referencing the erased MCPClient
    // (use-after-free on the next call).
    unregister_server_tools(reg, server);
    std::string err = mgr.connect(server);
    if (!err.empty()) return err;
    register_server_tools(reg, mgr, server);
    return "";
}

void mcp_disconnect(ServerManager& mgr, ToolRegistry& reg,
                    const std::string& server) {
    unregister_server_tools(reg, server);
    mgr.disconnect(server);
}

std::string mcp_refresh(ServerManager& mgr, ToolRegistry& reg,
                        const std::string& server) {
    std::string err = mgr.refresh(server);
    if (!err.empty()) return err;
    unregister_server_tools(reg, server);
    register_server_tools(reg, mgr, server);
    return "";
}

std::string mcp_enable(ServerManager& mgr, ToolRegistry& reg,
                       const std::string& server, bool on) {
    std::string err = mgr.set_enabled(server, on);
    if (!err.empty()) return err;
    if (!on) unregister_server_tools(reg, server);
    return "";
}

std::string mcp_trust(ServerManager& mgr, const std::string& server,
                      bool on) {
    return mgr.set_trusted(server, on);
}

std::string mcp_prompt(ServerManager& mgr, const std::string& server,
                       const std::string& name, const json& arguments,
                       std::string& out_text) {
    MCPClient* client = mgr.client(server);
    if (!client) return "server '" + server + "' not connected";
    const McpPromptDef* def = nullptr;
    for (const auto& p : client->prompts()) {
        if (p.name == name) {
            def = &p;
            break;
        }
    }
    if (!def) return "unknown prompt '" + name + "' on server '" + server +
                          "'";
    for (const auto& req : def->required_args) {
        if (arguments.contains(req)) continue;
        std::string err = "missing required argument '";
        err += req;
        err += "' for prompt '";
        err += name;
        err += "'";
        return err;
    }
    McpResult r = client->get_prompt(name, arguments);
    if (!r.ok) return r.error.empty() ? "prompt get failed" : r.error;
    out_text = r.text;
    return "";
}

} // namespace agent
