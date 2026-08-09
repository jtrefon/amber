
#include <cstdlib>
#include <memory>
#include <string>
#include <unistd.h>

#include "agent/mcp_client.h"
#include "agent/mcp_config.h"
#include "agent/mcp_tools.h"
#include "agent/registry.h"
#include "agent/workspace.h"
#include "mcp_test_util.h"
#include "tests/test_util.h"

namespace {

void run_cmd(const std::string& cmd) {
    int rc = std::system(cmd.c_str());
    (void)rc;
}

} // namespace

namespace {
using namespace mcp_test;
} // namespace

// Naming: sanitization, prefix, and the 64-char cap with hash suffix.
TEST(mcp_adapter_name_rules) {
    ASSERT_EQ(agent::mcp_adapter_name("github", "get_issue"),
              "mcp_github_get_issue");
    ASSERT_EQ(agent::mcp_adapter_name("a", "my.tool"),
              "mcp_a_my_tool");
    ASSERT_EQ(agent::mcp_adapter_name("a", "Bad Name!"),
              "mcp_a_Bad_Name_");
    std::string long_name = agent::mcp_adapter_name(
        "a-very-long-server-name", std::string(80, 'x'));
    ASSERT(long_name.size() <= 64u);
    ASSERT(long_name.rfind("mcp_a-very-long-server-name_", 0) == 0);
}

// An untrusted server's adapter requires approval and carries the marker;
// trust removes both. Never read-only regardless of annotations.
TEST(mcp_adapter_approval_and_trust) {
    auto ft = std::make_unique<FakeTransport>();
    FakeTransport* raw = ft.get();
    agent::MCPClient client("github", std::move(ft));
    script_connect(*raw);
    ASSERT_EQ(client.connect(), "");
    agent::McpToolDef def;
    def.name = "get_issue";
    def.description = "fetch an issue";
    def.input_schema = {{"type", "object"}};

    bool trusted = false;
    agent::McpToolAdapter adapter(
        client, def, "github", agent::mcp_adapter_name("github", "get_issue"),
        [&]() { return trusted; });
    ASSERT(adapter.requires_approval(json::object()));
    ASSERT_FALSE(adapter.is_read_only());
    ASSERT(adapter.description().find("[untrusted server]") == 0);

    trusted = true;
    ASSERT_FALSE(adapter.requires_approval(json::object()));
    ASSERT(adapter.description().find("[untrusted server]") ==
           std::string::npos);
}

// Adapter execution maps tool results and errors.
TEST(mcp_adapter_execute) {
    auto ft = std::make_unique<FakeTransport>();
    FakeTransport* raw = ft.get();
    agent::MCPClient client("github", std::move(ft));
    script_connect(*raw);
    ASSERT_EQ(client.connect(), "");
    agent::McpToolDef def;
    def.name = "get_issue";
    def.input_schema = {{"type", "object"}};

    agent::McpToolAdapter adapter(client, def, "github",
                                  agent::mcp_adapter_name("github",
                                                          "get_issue"),
                                  []() { return false; });

    raw->script.push_back(ok_result(
        {{"content", json::array(
             {{{"type", "text"}, {"text", "issue #1"}}})}}));
    auto ok = adapter.execute({{"number", 1}});
    ASSERT(ok.ok);
    ASSERT_EQ(ok.output, "issue #1");

    raw->script.push_back(err_result("unknown tool: get_issue"));
    auto bad = adapter.execute({{"number", 2}});
    ASSERT_FALSE(bad.ok);
    ASSERT(bad.error.find("unknown tool") != std::string::npos);
}

// register_server_tools wires real MCP tools into the registry; unregister
// removes them. End-to-end against the stdio echo fixture.
TEST(mcp_register_server_tools_end_to_end) {
    char buf[4096];
    std::string cwd = getcwd(buf, sizeof buf) ? buf : ".";
    std::string ws = "/tmp/amber_mcp_tools_ws";
    run_cmd("rm -rf " + ws);
    agent::Workspace::set_root(ws);
    agent::McpServerConfig cfg;
    cfg.name = "echo";
    cfg.type = "stdio";
    cfg.command = "python3";
    cfg.args = {"tests/fixtures/mcp_echo.py"};
    cfg.cwd = cwd;
    agent::ServerManager mgr({{"echo", cfg}});
    ASSERT_EQ(mgr.connect("echo"), "");

    agent::ToolRegistry reg;
    size_t n = agent::register_server_tools(reg, mgr, "echo");
    ASSERT_EQ(n, 1u);
    agent::Tool* tool = reg.find("mcp_echo_echo_tool");
    ASSERT(tool != nullptr);
    ASSERT(tool->is_read_only() == false);
    auto r = tool->execute({{"text", "hi"}});
    ASSERT(r.ok);
    ASSERT_EQ(r.output, "echo:hi");

    size_t removed = agent::unregister_server_tools(reg, "echo");
    ASSERT_EQ(removed, 1u);
    ASSERT(reg.find("mcp_echo_echo_tool") == nullptr);
    mgr.shutdown_all();
}

// read_resource reads a server resource; unknown servers error cleanly.
TEST(mcp_read_resource_tool) {
    char buf[4096];
    std::string cwd = getcwd(buf, sizeof buf) ? buf : ".";
    std::string ws = "/tmp/amber_mcp_tools_ws2";
    run_cmd("rm -rf " + ws);
    agent::Workspace::set_root(ws);
    agent::McpServerConfig cfg;
    cfg.name = "echo";
    cfg.type = "stdio";
    cfg.command = "python3";
    cfg.args = {"tests/fixtures/mcp_echo.py"};
    cfg.cwd = cwd;
    agent::ServerManager mgr({{"echo", cfg}});
    ASSERT_EQ(mgr.connect("echo"), "");

    auto tool = agent::make_read_resource_tool(mgr);
    ASSERT(tool->is_read_only());
    ASSERT_FALSE(tool->requires_approval(json::object()));
    auto r = tool->execute({{"server", "echo"}, {"uri", "doc://greet"}});
    ASSERT(r.ok);
    ASSERT(r.output.find("hello doc://greet") != std::string::npos);

    auto bad = tool->execute({{"server", "nope"}, {"uri", "doc://x"}});
    ASSERT_FALSE(bad.ok);
    ASSERT(bad.error.find("not connected") != std::string::npos);
    mgr.shutdown_all();
}

namespace {
// Decoy adapter that used to exist on the server but is no longer advertised
// — the reconnect path must drop it instead of leaving it dangling.
class DecoyTool : public agent::Tool {
public:
    std::string name() const noexcept override { return "mcp_echo_stale_tool"; }
    std::string description() const noexcept override { return "decoy"; }
    agent::json parameters_schema() const override {
        return agent::json::object();
    }
    agent::ToolResult execute(const agent::json&) const override {
        return {true, "ok", ""};
    }
};
} // namespace

// Reconnecting a server must unregister the old adapters first: a tool the
// new server no longer advertises would otherwise keep a stale McpToolAdapter
// bound to the erased MCPClient (use-after-free on the next call).
TEST(mcp_reconnect_server_tools_no_stale_adapters) {
    char buf[4096];
    std::string cwd = getcwd(buf, sizeof buf) ? buf : ".";
    std::string ws = "/tmp/amber_mcp_reconnect_ws";
    run_cmd("rm -rf " + ws);
    agent::Workspace::set_root(ws);
    agent::McpServerConfig cfg;
    cfg.name = "echo";
    cfg.type = "stdio";
    cfg.command = "python3";
    cfg.args = {"tests/fixtures/mcp_echo.py"};
    cfg.cwd = cwd;
    agent::ServerManager mgr({{"echo", cfg}});
    ASSERT_EQ(mgr.connect("echo"), "");

    agent::ToolRegistry reg;
    ASSERT_EQ(agent::register_server_tools(reg, mgr, "echo"), 1u);
    // A stale adapter with the server's prefix, left over from an older
    // tool set.
    reg.register_tool(std::make_unique<DecoyTool>());

    ASSERT_EQ(agent::reconnect_server_tools(reg, mgr, "echo"), 1u);
    ASSERT(reg.find("mcp_echo_echo_tool") != nullptr);
    ASSERT(reg.find("mcp_echo_stale_tool") == nullptr);
    mgr.shutdown_all();
}
