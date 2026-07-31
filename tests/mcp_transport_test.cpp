
#include <cerrno>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

#include "agent/mcp_transport.h"
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

} // namespace

// [MT-01] Echo round-trip over newline-framed stdio.
TEST(mcp_stdio_echo_roundtrip) {
    agent::StdioTransport t("python3", {"tests/fixtures/mcp_echo.py"}, ".",
                            {}, 5000);
    ASSERT_EQ(t.failure_reason(), "");

    auto resp = t.request(1, "initialize",
                          {{"protocolVersion", "2025-06-18"}});
    ASSERT(resp.has_value());
    ASSERT(resp->id.has_value());
    ASSERT_EQ(resp->id->dump(), "1");
    ASSERT(resp->result.has_value());
    ASSERT_EQ(resp->result->value("echo", json::object())
                 .value("method", ""),
              "initialize");

    auto r2 = t.request(2, "tools/list", json::object());
    ASSERT(r2.has_value());
    ASSERT_EQ(r2->id->dump(), "2");
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
    ASSERT(resp.has_value());
    ASSERT(resp->result.has_value());
    ASSERT(resp->result->dump().find("hello stderr") == std::string::npos);
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
    ASSERT(reason.find("spawn") != std::string::npos ||
           reason.find("closed") != std::string::npos);
    ASSERT_FALSE(t.request(1, "ping", json::object()).has_value());
    ASSERT_FALSE(t.notify("notifications/initialized", json::object()));
}
