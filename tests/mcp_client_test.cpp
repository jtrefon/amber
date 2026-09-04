
#include <chrono>
#include <csignal>
#include <fstream>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "agent/mcp_client.h"
#include "agent/mcp_transport_stdio.h"
#include "mcp_test_util.h"
#include "tests/test_util.h"

namespace {
using namespace mcp_test;
} // namespace

// [MP-01] Connect negotiates, discovers, and sends initialized.
TEST(mcp_client_connect_and_discover) {
    auto ft = std::make_unique<FakeTransport>();
    FakeTransport* raw = ft.get();
    agent::MCPClient client("fixture", std::move(ft));

    raw->script.push_back(ok_result(init_result()));
    raw->script.push_back(ok_result(
        {{"tools", json::array({{{"name", "get_issue"},
                                 {"description", "d"},
                                 {"inputSchema", {{"type", "object"}}}}})}}));
    raw->script.push_back(ok_result(
        {{"resources", json::array({{{"uri", "doc://x"}, {"name", "x"}}})}}));
    raw->script.push_back(ok_result(
        {{"prompts", json::array({{{"name", "review"},
                                   {"arguments", json::array(
                                       {{{"name", "code"},
                                         {"required", true}}})}}})}}));

    std::string err = client.connect();
    ASSERT_EQ(err, "");
    ASSERT(client.connected());
    ASSERT_EQ(client.tools().size(), 1u);
    ASSERT_EQ(client.tools()[0].name, "get_issue");
    ASSERT_EQ(client.resources().size(), 1u);
    ASSERT_EQ(client.prompts().size(), 1u);
    ASSERT_EQ(client.prompts()[0].required_args.size(), 1u);
    ASSERT_EQ(client.server_info().name, "fixture");
    ASSERT_EQ(raw->initialized_notifications, 1);
    ASSERT_EQ(raw->methods[0], "initialize");
}

// Version negotiation: an unsupported version disconnects with a typed error.
TEST(mcp_client_version_mismatch) {
    auto ft = std::make_unique<FakeTransport>();
    FakeTransport* raw = ft.get();
    agent::MCPClient client("fixture", std::move(ft));
    raw->script.push_back(ok_result(init_result("1999-01-01")));
    std::string err = client.connect();
    ASSERT_FALSE(err.empty());
    ASSERT(err.find("unsupported protocol version") != std::string::npos);
    ASSERT_FALSE(client.connected());
}

// [MP-02] call_tool flattens content blocks and maps isError.
TEST(mcp_client_call_tool_flattens_content) {
    auto ft = std::make_unique<FakeTransport>();
    FakeTransport* raw = ft.get();
    agent::MCPClient client("fixture", std::move(ft));
    script_connect(*raw);
    ASSERT_EQ(client.connect(), "");

    raw->script.push_back(ok_result(
        {{"content", json::array(
             {{{"type", "text"}, {"text", "hello "}},
              {{"type", "resource"},
               {"resource", {{"uri", "doc://y"}, {"text", "world"}}}},
              {{"type", "image"}, {"mimeType", "image/png"}, {"data", "abc"}}})}}));
    auto r = client.call_tool("get_issue", json::object());
    ASSERT(r.ok);
    ASSERT(r.text.find("hello ") != std::string::npos);
    ASSERT(r.text.find("[resource: doc://y]") != std::string::npos);
    ASSERT(r.text.find("world") != std::string::npos);
    ASSERT(r.text.find("[image image/png, 3 bytes]") != std::string::npos);

    raw->script.push_back(ok_result({{"isError", true},
                                     {"content", json::array(
                                         {{{"type", "text"},
                                           {"text", "boom"}}})}}));
    auto r2 = client.call_tool("get_issue", json::object());
    ASSERT_FALSE(r2.ok);
    ASSERT_EQ(r2.error, "boom");
}

// [MP-03] read_resource returns contents; get_prompt flattens messages.
TEST(mcp_client_read_resource_and_prompt) {
    auto ft = std::make_unique<FakeTransport>();
    FakeTransport* raw = ft.get();
    agent::MCPClient client("fixture", std::move(ft));
    script_connect(*raw);
    ASSERT_EQ(client.connect(), "");

    raw->script.push_back(ok_result(
        {{"contents", json::array(
             {{{"uri", "doc://arch"}, {"mimeType", "text/plain"},
               {"text", "the architecture"}}})}}));
    auto res = client.read_resource("doc://arch");
    ASSERT(res.ok);
    ASSERT(res.text.find("the architecture") != std::string::npos);

    raw->script.push_back(ok_result(
        {{"messages", json::array(
             {{{"role", "user"},
               {"content", {{"type", "text"}, {"text", "review this"}}}},
              {{"role", "assistant"},
               {"content", {{"type", "text"}, {"text", "done"}}}}})}}));
    auto p = client.get_prompt("review", {{"code", "x"}});
    ASSERT(p.ok);
    ASSERT(p.text.find("user: review this") != std::string::npos);
    ASSERT(p.text.find("assistant: done") != std::string::npos);
}

// Pagination: nextCursor drives a second page, capped at 10 pages.
TEST(mcp_client_discovery_pagination) {
    auto ft = std::make_unique<FakeTransport>();
    FakeTransport* raw = ft.get();
    agent::MCPClient client("fixture", std::move(ft));
    raw->script.push_back(ok_result(init_result()));
    raw->script.push_back(ok_result(
        {{"tools", json::array({{{"name", "t1"}}})},
         {"nextCursor", "p2"}}));
    raw->script.push_back(ok_result(
        {{"tools", json::array({{{"name", "t2"}}})}}));
    raw->script.push_back(ok_result({{"resources", json::array()}}));
    raw->script.push_back(ok_result({{"prompts", json::array()}}));
    ASSERT_EQ(client.connect(), "");
    ASSERT_EQ(client.tools().size(), 2u);
    int list_calls = 0;
    for (const auto& m : raw->methods)
        if (m == "tools/list") ++list_calls;
    ASSERT_EQ(list_calls, 2);
}

// Sampling requests are rejected with -32601 (no client capability declared).
TEST(mcp_client_rejects_server_requests) {
    auto ft = std::make_unique<FakeTransport>();
    FakeTransport* raw = ft.get();
    agent::MCPClient client("fixture", std::move(ft));
    script_connect(*raw);
    ASSERT_EQ(client.connect(), "");

    agent::McpMessage req;
    req.id = 42;
    req.method = "sampling/createMessage";
    req.params = {{"messages", json::array()}};
    raw->deliver_server_message(req);
    ASSERT_EQ(raw->responded_errors.size(), 1u);
    ASSERT_EQ(raw->responded_errors[0], -32601);
}

// listChanged notifications trigger re-discovery on the next operation.
TEST(mcp_client_listchanged_refreshes) {
    auto ft = std::make_unique<FakeTransport>();
    FakeTransport* raw = ft.get();
    agent::MCPClient client("fixture", std::move(ft));
    script_connect(*raw);
    ASSERT_EQ(client.connect(), "");

    agent::McpMessage note;
    note.method = "notifications/tools/list_changed";
    raw->deliver_server_message(note);

    raw->script.push_back(ok_result({{"tools", json::array()}}));
    raw->script.push_back(ok_result({{"resources", json::array()}}));
    raw->script.push_back(ok_result({{"prompts", json::array()}}));
    raw->script.push_back(ok_result({{"content", json::array()}}));
    auto r = client.call_tool("get_issue", json::object());
    ASSERT(r.ok);
    int list_calls = 0;
    for (const auto& m : raw->methods)
        if (m == "tools/list") ++list_calls;
    ASSERT_EQ(list_calls, 2);
}

// [MT-06] A session-expired request re-initializes, re-discovers, and retries.
TEST(mcp_client_session_expired_retries) {
    auto ft = std::make_unique<FakeTransport>();
    FakeTransport* raw = ft.get();
    agent::MCPClient client("fixture", std::move(ft));
    script_connect(*raw);
    ASSERT_EQ(client.connect(), "");

    agent::McpTransportResult expired;
    expired.status = agent::McpTransportStatus::SessionExpired;
    raw->script.push_back(std::move(expired));
    script_connect(*raw);  // re-initialize + discovery
    raw->script.push_back(ok_result({{"content", json::array()}}));
    auto r = client.call_tool("get_issue", json::object());
    ASSERT(r.ok);
    int init_calls = 0;
    for (const auto& m : raw->methods)
        if (m == "initialize") ++init_calls;
    ASSERT_EQ(init_calls, 2);
}

// Timeouts surface as typed errors.
TEST(mcp_client_call_timeout) {
    auto ft = std::make_unique<FakeTransport>();
    FakeTransport* raw = ft.get();
    agent::MCPClient client("fixture", std::move(ft));
    script_connect(*raw);
    ASSERT_EQ(client.connect(), "");
    raw->script.push_back(timeout_result());
    auto r = client.call_tool("get_issue", json::object());
    ASSERT_FALSE(r.ok);
    ASSERT(r.error.find("timed out") != std::string::npos);
}

// Unknown tool error maps to a typed error with the server's message.
TEST(mcp_client_unknown_tool_error) {
    auto ft = std::make_unique<FakeTransport>();
    FakeTransport* raw = ft.get();
    agent::MCPClient client("fixture", std::move(ft));
    script_connect(*raw);
    ASSERT_EQ(client.connect(), "");
    raw->script.push_back(err_result("unknown tool: nope"));
    auto r = client.call_tool("nope", json::object());
    ASSERT_FALSE(r.ok);
    ASSERT(r.error.find("unknown tool: nope") != std::string::npos);
}

// Flattening caps at the byte budget with an explicit truncation marker.
TEST(mcp_client_flatten_cap) {
    std::string big(70000, 'x');
    json content = json::array({{{ "type", "text"}, {"text", big}}});
    std::string flat = agent::mcp_flatten_content(
        content, static_cast<size_t>(64) * 1024);
    ASSERT(flat.find("[truncated:") != std::string::npos);
    ASSERT(flat.size() <= (static_cast<size_t>(64) * 1024) + 64);
}

// [MS-06] A pre-cancelled token fails fast without sending anything.
TEST(mcp_client_cancel_pre_request) {
    agent::CancellationToken token;
    token.request();
    auto ft = std::make_unique<FakeTransport>();
    FakeTransport* raw = ft.get();
    agent::MCPClient client("fixture", std::move(ft), "", &token);
    script_connect(*raw);
    ASSERT_EQ(client.connect(), "");
    auto r = client.call_tool("get_issue", json::object());
    ASSERT_FALSE(r.ok);
    ASSERT(r.error.find("cancelled") != std::string::npos);
    for (const auto& m : raw->methods)
        ASSERT(m != "tools/call");
}


// [MS-06] A hung stdio call is interrupted promptly by the shared token.
TEST(mcp_client_cancel_interrupts_hung_call) {
    agent::CancellationToken token;
    std::string pidfile = "/tmp/mcp_cancel_pid.txt";
    unlink(pidfile.c_str());
    agent::StdioTransport t(
        "tests/fixtures/mcp_ignore_sigterm",
        std::vector<std::string>{pidfile},
        ".", nullptr, 10000, &token);
    std::thread canceller([&]() {
        usleep(100 * 1000);
        token.request();
    });
    auto t0 = std::chrono::steady_clock::now();
    auto r = t.request(1, "initialize", json::object());
    canceller.join();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    ASSERT(r.status == agent::McpTransportStatus::Cancelled);
    ASSERT(elapsed.count() < 5000);
    std::ifstream f(pidfile);
    int pid = -1;
    f >> pid;
    if (pid > 0) {
        kill(pid, SIGKILL);
        int st = 0;
        waitpid(pid, &st, 0);
    }
    unlink(pidfile.c_str());
}
