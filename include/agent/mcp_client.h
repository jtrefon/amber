
#ifndef AGENT_MCP_CLIENT_H
#define AGENT_MCP_CLIENT_H

#include <functional>
#include <memory>
#include <optional>
#include <atomic>
#include <string>
#include <vector>

#include "agent/mcp_transport.h"
#include "agent/process.h"

namespace agent {

// A server's negotiated capability set (from the initialize result).
struct McpCapabilities {
    bool has_tools = false;
    bool has_resources = false;
    bool has_prompts = false;
    bool tools_list_changed = false;
    bool resources_list_changed = false;
    bool prompts_list_changed = false;
    bool has_logging = false;
};

struct McpToolDef {
    std::string name;
    std::string title;
    std::string description;
    json input_schema = json::object();
};

struct McpResourceDef {
    std::string uri;
    std::string name;
    std::string description;
    std::string mime_type;
};

struct McpPromptDef {
    std::string name;
    std::string title;
    std::string description;
    std::vector<std::string> required_args;
};

struct McpServerInfo {
    std::string name;
    std::string title;
    std::string version;
};

// The outcome of a primitive call (tool call, resource read, prompt get).
// Text is the flattened, capped payload; `ok=false` carries a tool error.
struct McpResult {
    std::string text;
    bool ok = true;
    std::string error;
};

// One client session against one MCP server. Owns the transport, runs
// initialize/negotiation and discovery, and exposes the three primitives.
// All mutation happens on the agent thread; the UI reads snapshots via the
// const accessors.
class MCPClient {
public:
    // Takes ownership of `transport` (any implementation). A null transport
    // yields a client whose connect() reports the given error. `cancel_token`
    // (optional, shared) aborts in-flight calls with a Cancelled result and
    // sends notifications/cancelled to the server.
    MCPClient(std::string server_name, std::unique_ptr<McpTransport> transport,
              std::string transport_error = "",
              const CancellationToken* cancel_token = nullptr);

    // initialize + version negotiation + discovery. Returns "" on success or
    // a human-readable error.
    std::string connect();

    // Re-run discovery for every declared capability (listChanged or
    // explicit refresh). Returns "" on success.
    std::string refresh();

    // Transport teardown; idempotent.
    void disconnect();

    bool connected() const { return connected_; }

    // Primitives.
    McpResult call_tool(const std::string& name, const json& arguments);
    McpResult read_resource(const std::string& uri);
    McpResult get_prompt(const std::string& name, const json& arguments);

    // Discovery snapshots.
    const std::vector<McpToolDef>& tools() const { return tools_; }
    const std::vector<McpResourceDef>& resources() const { return resources_; }
    const std::vector<McpPromptDef>& prompts() const { return prompts_; }
    const McpServerInfo& server_info() const { return server_info_; }
    const McpCapabilities& capabilities() const { return caps_; }

    // Last error (connect/discovery/call failures), "" when healthy.
    const std::string& error() const { return error_; }
    const std::string& name() const { return name_; }

private:
    std::string do_connect();
    McpTransportResult request_with_retry(int id, const std::string& method,
                                          const json& params);
    std::string discover_tools();
    std::string discover_resources();
    std::string discover_prompts();
    void handle_server_message(const McpMessage& msg);
    void notify_cancelled(int request_id);
    int next_id() { return id_counter_++; }

    std::string name_;
    std::unique_ptr<McpTransport> transport_;
    std::string transport_error_;
    const CancellationToken* cancel_token_ = nullptr;
    int id_counter_ = 1;
    bool connected_ = false;
    std::string error_;
    McpServerInfo server_info_;
    McpCapabilities caps_;
    std::vector<McpToolDef> tools_;
    std::vector<McpResourceDef> resources_;
    std::vector<McpPromptDef> prompts_;
    // Set by the transport thread (server notifications), read on the
    // agent thread — must be atomic.
    std::atomic<bool> list_changed_ = false;
};

// Flatten MCP tool/resource content blocks into one text payload, capped at
// `cap_bytes` (truncation is explicit). Shared by the client and adapters.
std::string mcp_flatten_content(const json& content, size_t cap_bytes);

} // namespace agent

#endif // AGENT_MCP_CLIENT_H
