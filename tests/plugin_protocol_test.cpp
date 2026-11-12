// The external plugin protocol is newline-framed JSON. A plugin is a child
// process the harness does not control, so framing and decoding are pure
// functions with no file descriptors: the unit suite and the fuzz target drive
// the same code the manager reads bytes into.

#include "agent/plugin_protocol.h"

#include "test_util.h"

#include <string>

using agent::PluginCallReply;
using agent::PluginInitReply;
using agent::PluginLineState;

namespace {

std::string cap_sized(char fill, std::size_t extra = 0) {
    return std::string(agent::kPluginMaxLineBytes + extra, fill);
}

} // namespace

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

TEST(plugin_framing_waits_for_a_newline) {
    std::string buffer = "{\"partial\":";
    std::string line = "untouched";
    ASSERT(agent::plugin_take_line(buffer, line) == PluginLineState::Incomplete);
    ASSERT_EQ(line, std::string("untouched"));
    ASSERT_EQ(buffer, std::string("{\"partial\":"));
}

TEST(plugin_framing_takes_one_line_and_keeps_the_rest) {
    std::string buffer = "first\nsecond\n";
    std::string line;
    ASSERT(agent::plugin_take_line(buffer, line) == PluginLineState::Ready);
    ASSERT_EQ(line, std::string("first"));
    ASSERT_EQ(buffer, std::string("second\n"));
    ASSERT(agent::plugin_take_line(buffer, line) == PluginLineState::Ready);
    ASSERT_EQ(line, std::string("second"));
    ASSERT_EQ(buffer, std::string(""));
}

TEST(plugin_framing_accepts_an_empty_line) {
    std::string buffer = "\n";
    std::string line = "stale";
    ASSERT(agent::plugin_take_line(buffer, line) == PluginLineState::Ready);
    ASSERT_EQ(line, std::string(""));
}

TEST(plugin_framing_leaves_cr_for_the_decoder) {
    // CRLF is common on Windows plugins. The framing strips only '\n'; the JSON
    // parser accepts the trailing '\r' as whitespace, so the decoder below is
    // where the two meet.
    std::string buffer = "{\"result\":{\"ok\":true}}\r\n";
    std::string line;
    ASSERT(agent::plugin_take_line(buffer, line) == PluginLineState::Ready);
    ASSERT_EQ(line, std::string("{\"result\":{\"ok\":true}}\r"));
    ASSERT(agent::plugin_decode_initialize(line).ok);
}

TEST(plugin_framing_allows_a_line_exactly_at_the_cap) {
    std::string buffer = cap_sized('a') + "\n";
    std::string line;
    ASSERT(agent::plugin_take_line(buffer, line) == PluginLineState::Ready);
    ASSERT_EQ(line.size(), agent::kPluginMaxLineBytes);
}

TEST(plugin_framing_refuses_an_unterminated_cap_breaker) {
    std::string buffer = cap_sized('a', 1);
    std::string line;
    ASSERT(agent::plugin_take_line(buffer, line) == PluginLineState::TooLarge);
}

TEST(plugin_framing_refuses_a_terminated_cap_breaker) {
    std::string buffer = cap_sized('a', 1) + "\n";
    std::string line;
    ASSERT(agent::plugin_take_line(buffer, line) == PluginLineState::TooLarge);
}

// ---------------------------------------------------------------------------
// Initialize reply
// ---------------------------------------------------------------------------

TEST(plugin_initialize_accepts_the_handshake) {
    const PluginInitReply reply =
        agent::plugin_decode_initialize("{\"id\":1,\"result\":{\"ok\":true}}");
    ASSERT(reply.ok);
    ASSERT_EQ(reply.error, std::string(""));
}

TEST(plugin_initialize_rejects_ok_false) {
    const PluginInitReply reply = agent::plugin_decode_initialize("{\"result\":{\"ok\":false}}");
    ASSERT(!reply.ok);
    ASSERT_EQ(reply.error, std::string("initialize rejected"));
}

TEST(plugin_initialize_rejects_a_missing_result) {
    ASSERT(!agent::plugin_decode_initialize("{\"id\":1}").ok);
    ASSERT(!agent::plugin_decode_initialize("[]").ok);
    ASSERT(!agent::plugin_decode_initialize("not json").ok);
}

TEST(plugin_initialize_adopts_a_typed_boolean) {
    // The protocol prefers real booleans, but a plugin that spells it "true" or
    // 1 must not be rejected for it; read_bool already sets that contract.
    ASSERT(agent::plugin_decode_initialize("{\"result\":{\"ok\":\"true\"}}").ok);
    ASSERT(agent::plugin_decode_initialize("{\"result\":{\"ok\":1}}").ok);
    ASSERT(!agent::plugin_decode_initialize("{\"result\":{\"ok\":\"no\"}}").ok);
}

// ---------------------------------------------------------------------------
// Tool-call reply
// ---------------------------------------------------------------------------

TEST(plugin_call_decodes_output_and_meta) {
    const PluginCallReply reply = agent::plugin_decode_call(
        "{\"id\":2,\"result\":{\"ok\":true,\"output\":\"42\",\"meta\":{\"ms\":3}}}");
    ASSERT(reply.decoded);
    ASSERT(reply.ok);
    ASSERT_EQ(reply.output, std::string("42"));
    ASSERT_EQ(reply.error, std::string(""));
    ASSERT_EQ(reply.meta["ms"].get<int>(), 3);
}

TEST(plugin_call_reads_the_error_field_when_output_is_empty) {
    const PluginCallReply reply =
        agent::plugin_decode_call("{\"result\":{\"ok\":false,\"error\":\"boom\"}}");
    ASSERT(reply.decoded);
    ASSERT(!reply.ok);
    ASSERT_EQ(reply.output, std::string("boom"));
}

TEST(plugin_call_prefers_output_over_error) {
    const PluginCallReply reply =
        agent::plugin_decode_call("{\"result\":{\"ok\":false,\"output\":\"partial\",\"error\":\"boom\"}}");
    ASSERT(reply.decoded);
    ASSERT_EQ(reply.output, std::string("partial"));
}

TEST(plugin_call_fails_closed_on_malformed_json) {
    const PluginCallReply reply = agent::plugin_decode_call("{\"result\":");
    ASSERT(!reply.decoded);
    ASSERT_EQ(reply.error, std::string("malformed plugin response"));
}

TEST(plugin_call_fails_closed_on_a_missing_result) {
    ASSERT(!agent::plugin_decode_call("{\"id\":2}").decoded);
    ASSERT(!agent::plugin_decode_call("[]").decoded);
    ASSERT(!agent::plugin_decode_call("").decoded);
}

TEST(plugin_call_tolerates_a_typeless_result) {
    // `result` present but an array: the old decoder produced an unsuccessful
    // ToolResult with an empty message rather than a protocol error. Keep that,
    // so a plugin that answers oddly still fails only its own call.
    const PluginCallReply reply = agent::plugin_decode_call("{\"result\":[]}");
    ASSERT(reply.decoded);
    ASSERT(!reply.ok);
    ASSERT_EQ(reply.output, std::string(""));
}
