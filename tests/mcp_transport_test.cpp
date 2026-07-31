
#include <string>

#include "agent/mcp_transport.h"
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
    ASSERT(msg->result->is_array());
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
