
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "agent/mcp_client.h"
#include "tests/test_util.h"

namespace {

// Scripted in-memory transport: every request pops the next scripted result.
class FakeTransport : public agent::McpTransport {
public:
    std::vector<std::string> methods;
    std::vector<int> responded_errors;
    int initialized_notifications = 0;
    std::deque<agent::McpTransportResult> script;
    bool fail_requests = false;

    agent::McpTransportResult request(int id, const std::string& method,
                                      const json& params) override {
        (void)id;
        (void)params;
        methods.push_back(method);
        if (fail_requests) {
            agent::McpTransportResult r;
            r.status = agent::McpTransportStatus::TransportError;
            return r;
        }
        if (script.empty()) {
            agent::McpTransportResult r;
            r.status = agent::McpTransportStatus::TransportError;
            return r;
        }
        agent::McpTransportResult r = std::move(script.front());
        script.pop_front();
        return r;
    }

    bool notify(const std::string& method, const json& params) override {
        (void)params;
        if (method == "notifications/initialized") ++initialized_notifications;
        return true;
    }

    bool respond(int id, const json& result) override {
        (void)id;
        (void)result;
        return true;
    }

    bool respond_error(int id, const agent::McpError& error) override {
        (void)id;
        responded_errors.push_back(error.code);
        return true;
    }

    void shutdown() override {}
    std::string failure_reason() const override {
        return fail_requests ? "fake failure" : "";
    }

    void deliver_server_message(const agent::McpMessage& m) {
        if (on_server_message_) on_server_message_(m);
    }
};

agent::McpTransportResult ok_result(json result) {
    agent::McpTransportResult r;
    agent::McpMessage m;
    m.id = 1;
    m.result = std::move(result);
    r.message = std::move(m);
    return r;
}

agent::McpTransportResult err_result(const std::string& message) {
    agent::McpTransportResult r;
    agent::McpMessage m;
    m.id = 1;
    agent::McpError e;
    e.code = -32602;
    e.message = message;
    m.error = e;
    r.message = std::move(m);
    return r;
}

agent::McpTransportResult timeout_result() {
    agent::McpTransportResult r;
    r.status = agent::McpTransportStatus::Timeout;
    return r;
}

json init_result(const std::string& version = "2025-06-18") {
    return {{"protocolVersion", version},
            {"capabilities",
             {{"tools", {{"listChanged", true}}},
              {"resources", json::object()},
              {"prompts", json::object()}}},
            {"serverInfo", {{"name", "fixture"}, {"version", "1.0"}}}};
}

// Script the connect sequence: initialize + three discovery pages.
void script_connect(FakeTransport& t) {
    t.script.push_back(ok_result(init_result()));
    t.script.push_back(ok_result({{"tools", json::array()}}));
    t.script.push_back(ok_result({{"resources", json::array()}}));
    t.script.push_back(ok_result({{"prompts", json::array()}}));
}

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
    std::string flat = agent::mcp_flatten_content(content, 64u * 1024);
    ASSERT(flat.find("[truncated:") != std::string::npos);
    ASSERT(flat.size() <= 64u * 1024 + 64);
}
