
#ifndef AGENT_MCP_TOOLS_H
#define AGENT_MCP_TOOLS_H

#include <functional>
#include <memory>
#include <string>

#include "agent/mcp_config.h"
#include "agent/registry.h"
#include "agent/tool.h"

namespace agent {

// Adapter exposing one server tool to the model as an ordinary amber Tool:
// name `mcp_<server>_<tool>`, never read-only (annotations are untrusted),
// approval-gated unless the server is trusted, schema passthrough.
class McpToolAdapter : public Tool {
public:
    McpToolAdapter(MCPClient& client, McpToolDef def,
                   std::string server_name, std::string adapter_name,
                   std::function<bool()> is_trusted);

    std::string name() const noexcept override { return name_; }
    bool is_read_only() const noexcept override { return false; }
    bool requires_approval(const json&) const noexcept override {
        return !is_trusted_();
    }
    std::string description() const noexcept override;
    json parameters_schema() const override;
    std::string summarize(const json& arguments) const override;
    ToolResult execute(const json& arguments) const override;

private:
    MCPClient& client_;
    McpToolDef def_;
    std::string server_name_;
    std::string name_;
    std::function<bool()> is_trusted_;
};

// Built-in read_resource tool: fetch a server resource by uri (user and model
// surface). Text-only in v1; blobs are rejected.
std::unique_ptr<Tool> make_read_resource_tool(ServerManager& mgr);

// Sanitize a server tool into the `mcp_<server>_<tool>` adapter name
// (non [a-zA-Z0-9_-] -> '_'; whole name capped at 64 chars with a 3-char
// suffix hash when truncated).
std::string mcp_adapter_name(const std::string& server,
                             const std::string& tool);

// Register one adapter per discovered tool of a connected server. Collisions
// (existing names) get a numeric suffix. Returns the number registered.
size_t register_server_tools(ToolRegistry& reg, ServerManager& mgr,
                             const std::string& server);

// Unregister every adapter of a server (disconnect/disable).
size_t unregister_server_tools(ToolRegistry& reg, const std::string& server);

} // namespace agent

#endif // AGENT_MCP_TOOLS_H
