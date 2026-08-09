
#include "agent/mcp_tools.h"

#include <algorithm>
#include <cctype>

namespace agent {

namespace {

constexpr size_t kMaxToolName = 64;

unsigned hash3(const std::string& s) {
    unsigned h = 2166136261u;
    for (char c : s) {
        h ^= static_cast<unsigned char>(c);
        h *= 16777619u;
    }
    return h & 0xFFFu;
}

std::string sanitize(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-';
        out += ok ? c : '_';
    }
    return out;
}

} // namespace

McpToolAdapter::McpToolAdapter(MCPClient& client, McpToolDef def,
                               std::string server_name,
                               std::string adapter_name,
                               std::function<bool()> is_trusted)
    : client_(client),
      def_(std::move(def)),
      server_name_(std::move(server_name)),
      name_(std::move(adapter_name)),
      is_trusted_(std::move(is_trusted)) {}
std::string McpToolAdapter::description() const noexcept {
    std::string out;
    if (!is_trusted_()) out = "[untrusted server] ";
    out += def_.description;
    return out;
}

json McpToolAdapter::parameters_schema() const {
    json schema = def_.input_schema;
    if (!schema.is_object() || schema.empty()) {
        schema = {{"type", "object"}, {"properties", json::object()}};
    }
    return schema;
}

std::string McpToolAdapter::summarize(const json& arguments) const {
    return "mcp " + server_name_ + ": " + def_.name + "(" +
           std::to_string(arguments.size()) + " args)";
}

ToolResult McpToolAdapter::execute(const json& arguments) const {
    McpResult r = client_.call_tool(def_.name, arguments);
    ToolResult out;
    if (!r.ok) {
        out.ok = false;
        out.error = r.error.empty()
            ? "mcp tool '" + def_.name + "' failed" : r.error;
        return out;
    }
    out.output = r.text;
    out.meta = {{"server", server_name_}, {"tool", def_.name}};
    return out;
}

std::string mcp_adapter_name(const std::string& server,
                             const std::string& tool) {
    std::string full = "mcp_" + sanitize(server) + "_" + sanitize(tool);
    if (full.size() <= kMaxToolName) return full;
    std::string head = full.substr(0, kMaxToolName - 4);
    char buf[8];
    std::snprintf(buf, sizeof buf, "_%03x", hash3(tool));
    return head + buf;
}

size_t register_server_tools(ToolRegistry& reg, ServerManager& mgr,
                             const std::string& server) {
    MCPClient* client = mgr.client(server);
    if (!client) return 0;
    size_t n = 0;
    for (const auto& def : client->tools()) {
        std::string base = mcp_adapter_name(server, def.name);
        std::string name = base;
        int suffix = 2;
        while (reg.find(name)) name = base + "_" + std::to_string(suffix++);
        auto adapter = std::make_unique<McpToolAdapter>(
            *client, def, server, name,
            [&mgr, server]() { return mgr.trusted(server); });
        reg.register_tool(std::move(adapter));
        ++n;
    }
    return n;
}

size_t unregister_server_tools(ToolRegistry& reg, const std::string& server) {
    return reg.unregister_tools_with_prefix("mcp_" + sanitize(server) + "_");
}

namespace {

// read_resource: fetch a server resource by uri (user and model surface).
class ReadResourceTool : public Tool {
public:
    explicit ReadResourceTool(ServerManager& mgr) : mgr_(mgr) {}

    std::string name() const noexcept override { return "read_resource"; }

    bool is_read_only() const noexcept override { return true; }

    std::string description() const noexcept override {
        return "Read a resource from a connected MCP server by uri "
               "(e.g. doc://architecture). Text content only in v1; binary "
               "resources are rejected.";
    }

    json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties",
             {{"server", {{"type", "string"},
                          {"description", "MCP server name"}}},
              {"uri", {{"type", "string"},
                       {"description", "Resource uri to read"}}}}},
            {"required", {"server", "uri"}}};
    }

    std::string summarize(const json& a) const override {
        return "mcp read_resource: " + a.value("uri", "?") + " from " +
               a.value("server", "?");
    }

    ToolResult execute(const json& a) const override {
        ToolResult out;
        std::string server = a.value("server", "");
        std::string uri = a.value("uri", "");
        if (server.empty() || uri.empty()) {
            out.ok = false;
            out.error = "missing 'server' or 'uri'";
            return out;
        }
        MCPClient* client = mgr_.client(server);
        if (!client) {
            out.ok = false;
            out.error = "mcp server '" + server + "' not connected";
            return out;
        }
        McpResult r = client->read_resource(uri);
        if (!r.ok) {
            out.ok = false;
            out.error = r.error.empty()
                ? "resource read failed" : r.error;
            return out;
        }
        out.output = r.text;
        out.meta = {{"server", server}, {"uri", uri}};
        return out;
    }

private:
    ServerManager& mgr_;
};

} // namespace

std::unique_ptr<Tool> make_read_resource_tool(ServerManager& mgr) {
    return std::make_unique<ReadResourceTool>(mgr);
}

json mcp_completion_subtree(const ToolRegistry& reg) {
    json mcp_node = {
        {"action", "core.mcp"},
        {"help", "Manage MCP servers."},
        {"man", "The mcp command manages MCP servers and the tools they "
                "expose. Live server tools appear under their server name."},
        {"children", json::object()}};
    json& children = mcp_node["children"];
    for (const auto& t : reg.snapshot_tools()) {
        std::string n = t->name();
        if (n.rfind("mcp_", 0) != 0) continue;
        std::string rest = n.substr(4);
        size_t dot = rest.find('_');
        if (dot == std::string::npos) continue;
        std::string server = rest.substr(0, dot);
        std::string tool = rest.substr(dot + 1);
        if (!children.contains(server)) {
            std::string action = "mcp." + server;
            std::string help = "Tools exposed by ";
            help += server;
            help += ".";
            std::string man = "Live tools of the MCP server ";
            man += server;
            man += ".";
            children[server] = {{"action", action},
                                {"help", help},
                                {"man", man},
                                {"children", json::object()}};
        }
        std::string tool_action = "mcp.";
        tool_action += server;
        tool_action += ".";
        tool_action += tool;
        children[server]["children"][tool] = {
            {"action", tool_action},
            {"help", t->description()},
            {"man", t->description()}};
    }
    return {{"mcp", mcp_node}};
}
} // namespace agent

