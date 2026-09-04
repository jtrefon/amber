
#include <cerrno>
#include <csignal>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

#include "agent/mcp_transport.h"
#include "agent/mcp_transport_http.h"
#include "agent/mcp_transport_stdio.h"
#include "tests/test_util.h"

// [MT-01] Encode/decode round-trip for requests, notifications, responses,
// and error responses over the JSON-RPC 2.0 envelope.
TEST(mcp_wire_request_roundtrip) {
    agent::McpRequest req;
    req.id = 7;
    req.method = "tools/list";
    req.params = {{"cursor", "abc"}};
    std::string wire = agent::mcp_encode_request(req);
    auto msg = agent::mcp_decode_line(wire);
    ASSERT(msg.has_value());
    ASSERT(msg->id.has_value());
    ASSERT_EQ(msg->id->dump(), "7");
    ASSERT_EQ(msg->method, "tools/list");
    ASSERT_EQ(msg->params.value("cursor", ""), "abc");
    ASSERT_FALSE(msg->is_notification());
}

TEST(mcp_wire_notification_roundtrip) {
    std::string wire = agent::mcp_encode_notification(
        "notifications/initialized", json::object());
    auto msg = agent::mcp_decode_line(wire);
    ASSERT(msg.has_value());
    ASSERT_FALSE(msg->id.has_value());
    ASSERT_EQ(msg->method, "notifications/initialized");
    ASSERT(msg->is_notification());
}

TEST(mcp_wire_response_roundtrip) {
    std::string wire = agent::mcp_encode_response(
        3, {{"tools", json::array()}});
    auto msg = agent::mcp_decode_line(wire);
    ASSERT(msg.has_value());
    ASSERT(msg->id.has_value());
    ASSERT_EQ(msg->id->dump(), "3");
    ASSERT(msg->result.has_value());
    ASSERT(msg->result->is_object());
    ASSERT((*msg->result)["tools"].is_array());
    ASSERT_FALSE(msg->error.has_value());
}

TEST(mcp_wire_error_response_roundtrip) {
    agent::McpError err;
    err.code = -32602;
    err.message = "bad params";
    err.data = {{"detail", "x"}};
    std::string wire = agent::mcp_encode_error_response(9, err);
    auto msg = agent::mcp_decode_line(wire);
    ASSERT(msg.has_value());
    ASSERT(msg->error.has_value());
    ASSERT_EQ(msg->error->code, -32602);
    ASSERT_EQ(msg->error->message, "bad params");
    ASSERT_EQ(msg->error->data.value("detail", ""), "x");
    ASSERT_FALSE(msg->result.has_value());
}

// Unknown fields are ignored (forward compatibility).
TEST(mcp_wire_tolerates_unknown_fields) {
    auto msg = agent::mcp_decode_line(
        R"({"jsonrpc":"2.0","id":1,"method":"ping","params":{},"future_key":42})");
    ASSERT(msg.has_value());
    ASSERT_EQ(msg->method, "ping");
}

// Invalid input: non-JSON, arrays, empty objects, empty strings.
TEST(mcp_wire_rejects_invalid_input) {
    ASSERT_FALSE(agent::mcp_decode_line("not json").has_value());
    ASSERT_FALSE(agent::mcp_decode_line("[]").has_value());
    ASSERT_FALSE(agent::mcp_decode_line("{}").has_value());
    ASSERT_FALSE(agent::mcp_decode_line("").has_value());
    ASSERT_FALSE(agent::mcp_decode_line("{\"jsonrpc\":\"2.0\"}").has_value());
}

// stdio framing: encoded messages must not contain raw newlines even when
// payload strings do (they are JSON-escaped).
TEST(mcp_wire_compact_no_embedded_newlines) {
    agent::McpRequest req;
    req.id = 1;
    req.method = "tools/call";
    req.params = {{"arguments", {{"text", "line1\nline2"}}}};
    std::string wire = agent::mcp_encode_request(req);
    ASSERT(wire.find('\n') == std::string::npos);
    auto msg = agent::mcp_decode_line(wire);
    ASSERT(msg.has_value());
    ASSERT_EQ(msg->params["arguments"].value("text", ""), "line1\nline2");
}

// Response ids may be strings per JSON-RPC; decode must tolerate them.
TEST(mcp_wire_string_id_tolerated) {
    auto msg = agent::mcp_decode_line(
        R"({"jsonrpc":"2.0","id":"abc","result":{}})");
    ASSERT(msg.has_value());
    ASSERT(msg->id.has_value());
    ASSERT_EQ(msg->id->dump(), "\"abc\"");
}

// ---------------------------------------------------------------------------
// stdio transport ([MT-01] echo round-trip, [MT-02] SIGKILL escalation,
// [MT-03] stderr isolation)
// ---------------------------------------------------------------------------

namespace {

bool wait_for_file(const std::string& path, int attempts = 100) {
    for (int i = 0; i < attempts; ++i) {
        std::ifstream f(path);
        if (f.is_open()) return true;
        usleep(10 * 1000);
    }
    return false;
}

// Start the HTTP fixture server; returns "http://127.0.0.1:<port>/mcp".
// Kills the fixture at scope end.
struct HttpFixture {
    std::string url;
    std::string statefile;
    int pid = -1;

    explicit HttpFixture(const std::string& mode)
        : statefile("/tmp/mcp_http_" + mode + ".txt") {
        unlink(statefile.c_str());
        std::string cmd = "python3 tests/fixtures/mcp_http_server.py " +
                          statefile + " " + mode + " >/dev/null 2>&1 &";
        ASSERT(std::system(cmd.c_str()) == 0);
        ASSERT(wait_for_file(statefile));
        std::ifstream f(statefile);
        std::string line;
        int port = -1;
        while (std::getline(f, line)) {
            if (line.rfind("PORT:", 0) == 0)
                port = std::stoi(line.substr(5));
            if (line.rfind("PID:", 0) == 0)
                pid = std::stoi(line.substr(4));
        }
        ASSERT(port > 0);
        ASSERT(pid > 0);
        url = "http://127.0.0.1:" + std::to_string(port) + "/mcp";
    }

    ~HttpFixture() {
        if (pid > 0) {
            kill(pid, SIGKILL);
            int st = 0;
            waitpid(pid, &st, 0);
        }
        unlink(statefile.c_str());
    }
};

} // namespace

// [MT-01] Echo round-trip over newline-framed stdio.
TEST(mcp_stdio_echo_roundtrip) {
    agent::StdioTransport t("python3", {"tests/fixtures/mcp_echo.py"}, ".",
                            {}, 5000);
    ASSERT_EQ(t.failure_reason(), "");

    auto resp = t.request(1, "initialize",
                          {{"protocolVersion", "2025-06-18"}});
    ASSERT(resp.status == agent::McpTransportStatus::Ok);
    ASSERT(resp.message.has_value());
    ASSERT(resp.message->id.has_value());
    ASSERT_EQ(resp.message->id->dump(), "1");
    ASSERT(resp.message->result.has_value());
    ASSERT_EQ(resp.message->result->value("protocolVersion", ""),
              "2025-06-18");

    auto r2 = t.request(2, "tools/list", json::object());
    ASSERT(r2.status == agent::McpTransportStatus::Ok);
    ASSERT(r2.message.has_value());
    const agent::McpMessage& msg2 = *r2.message;
    ASSERT(msg2.id.has_value());
    ASSERT_EQ(msg2.id->dump(), "2");
    ASSERT_TRUE(t.notify("notifications/initialized", json::object()));
    t.shutdown();
    ASSERT_FALSE(t.failure_reason().empty());
}

// [MT-02] A server that ignores SIGTERM is escalated to SIGKILL and reaped.
TEST(mcp_stdio_sigterm_escalates_to_sigkill) {
    std::string pidfile = "/tmp/mcp_sigterm_pid.txt";
    unlink(pidfile.c_str());
    {
        agent::StdioTransport t(
            "python3",
            {"tests/fixtures/mcp_ignore_sigterm.py", pidfile}, ".", {}, 5000);
        ASSERT(wait_for_file(pidfile));
        int pid = -1;
        {
            std::ifstream f(pidfile);
            f >> pid;
        }
        ASSERT(pid > 0);
        t.shutdown();
        int st = 0;
        ASSERT_EQ(waitpid(pid, &st, WNOHANG), -1);
        ASSERT_EQ(errno, ECHILD);
    }
}

// [MT-03] stderr never pollutes the protocol; it is retained for diagnostics.
TEST(mcp_stdio_stderr_isolated) {
    agent::StdioTransport t(
        "python3", {"tests/fixtures/mcp_echo.py", "stderr"}, ".", {}, 5000);
    auto resp = t.request(1, "ping", json::object());
    ASSERT(resp.status == agent::McpTransportStatus::Ok);
    auto m = resp.message;
    ASSERT(m.has_value());
    ASSERT(m->result.has_value());
    ASSERT(m->result->dump().find("hello stderr") == std::string::npos);
}

// A server that dies at startup surfaces its stderr in failure_reason().
TEST(mcp_stdio_startup_failure_reports_stderr) {
    agent::StdioTransport t(
        "python3", {"tests/fixtures/mcp_echo.py", "boom"}, ".", {}, 2000);
    std::string reason;
    for (int i = 0; i < 200; ++i) {
        reason = t.failure_reason();
        if (!reason.empty()) break;
        usleep(10 * 1000);
    }
    ASSERT_FALSE(reason.empty());
    ASSERT(reason.find("fatal startup error") != std::string::npos);
}

// A missing server binary fails fast with a spawn error, never hangs.
TEST(mcp_stdio_spawn_failure_fails_fast) {
    agent::StdioTransport t("no-such-mcp-server-binary", {}, ".", {}, 2000);
    std::string reason;
    for (int i = 0; i < 200; ++i) {
        reason = t.failure_reason();
        if (!reason.empty()) break;
        usleep(10 * 1000);
    }
    ASSERT_FALSE(reason.empty());
    bool spawn_or_closed = reason.find("spawn") != std::string::npos ||
                           reason.find("closed") != std::string::npos;
    ASSERT(spawn_or_closed);
    auto r = t.request(1, "ping", json::object());
    ASSERT(r.status == agent::McpTransportStatus::TransportError);
    ASSERT_FALSE(r.message.has_value());
    ASSERT_FALSE(t.notify("notifications/initialized", json::object()));
}

// ---------------------------------------------------------------------------
// Streamable HTTP transport ([MT-04] JSON round-trip, [MT-05] SSE streaming,
// [MT-06] session expiry)
// ---------------------------------------------------------------------------

// [MT-04] application/json round-trip with protocol/session headers.
TEST(mcp_http_json_roundtrip) {
    HttpFixture fx("echo");
    agent::HttpTransport t(fx.url, "sekret", 5000);
    auto r = t.request(1, "initialize",
                       {{"protocolVersion", "2025-06-18"}});
    ASSERT(r.status == agent::McpTransportStatus::Ok);
    ASSERT(r.message.has_value());
    const agent::McpMessage& m0 = *r.message;
    ASSERT(m0.id.has_value());
    ASSERT_EQ(m0.id->dump(), "1");
    ASSERT(m0.result.has_value());
    ASSERT_EQ(m0.result->value("protocolVersion", ""), "2025-06-18");

    auto r2 = t.request(2, "tools/list", json::object());
    ASSERT(r2.status == agent::McpTransportStatus::Ok);
    ASSERT(r2.message.has_value());
    const agent::McpMessage& m2 = *r2.message;
    ASSERT(m2.result.has_value());
    ASSERT_EQ(m2.result->value("echo", json::object()).value("method", ""),
              "tools/list");
    ASSERT_TRUE(t.notify("notifications/initialized", json::object()));
    t.close_session();
    t.shutdown();
    ASSERT_EQ(t.failure_reason(), "");
}

// [MT-05] SSE responses deliver interleaved server messages, then the reply.
TEST(mcp_http_sse_streaming_response) {
    HttpFixture fx("sse");
    int server_msgs = 0;
    agent::HttpTransport t(
        fx.url, "", 5000,
        [&](const agent::McpMessage& m) {
            if (m.method == "notifications/progress") ++server_msgs;
        });
    auto r = t.request(7, "tools/call",
                       {{"name", "x"}, {"arguments", json::object()}});
    ASSERT(r.status == agent::McpTransportStatus::Ok);
    auto m = r.message;
    ASSERT(m.has_value());
    ASSERT(m->result.has_value());
    ASSERT_EQ(m->result->value("sse", false), true);
    ASSERT_EQ(server_msgs, 1);
}

// [MT-06] A stale session surfaces as SessionExpired for the client to retry.
TEST(mcp_http_session_expiry) {
    HttpFixture fx("session");
    agent::HttpTransport t(fx.url, "", 5000);
    auto init = t.request(1, "initialize", json::object());
    ASSERT(init.status == agent::McpTransportStatus::Ok);
    ASSERT_EQ(t.session_id(), "sess-1");

    auto r2 = t.request(2, "tools/list", json::object());
    ASSERT(r2.status == agent::McpTransportStatus::Ok);
    ASSERT(r2.message.has_value());
    const agent::McpMessage& m2 = *r2.message;
    ASSERT(m2.result.has_value());
    ASSERT_EQ(m2.result->value("session", ""), "sess-1");

    auto r3 = t.request(3, "tools/call", json::object());
    ASSERT(r3.status == agent::McpTransportStatus::Ok);

    auto r4 = t.request(4, "ping", json::object());
    ASSERT(r4.status == agent::McpTransportStatus::SessionExpired);
    t.close_session();
}

// A request without a matching response times out with a typed status.
TEST(mcp_http_bad_url_fails_fast) {
    agent::HttpTransport t("http://127.0.0.1:1/mcp", "", 2000);
    auto r = t.request(1, "ping", json::object());
    ASSERT(r.status == agent::McpTransportStatus::TransportError);
    ASSERT_FALSE(t.failure_reason().empty());
}
