
// Anthropic Messages API dialect — hermetic tests (FIX-030).
//
// The dialect is the growth proof for the provider seam: a completely
// different wire protocol implemented in one file, with recorded JSON
// fixtures and no network. These tests pin the translation between the
// internal message model and the Messages API shape in both directions.

#include "agent/dialect.h"
#include "agent/dialect_anthropic.h"
#include "agent/tools.h"
#include "test_util.h"

#include <memory>
#include <string>
#include <vector>

namespace {

std::unique_ptr<agent::Dialect> anthropic() {
    return agent::make_anthropic_dialect();
}

agent::Message text_msg(const std::string& role, const std::string& text) {
    agent::Message m;
    m.role = role;
    m.content = text;
    return m;
}

agent::Message tool_result_msg(const std::string& id, const std::string& text) {
    agent::Message m;
    m.role = "tool";
    m.tool_call_id = id;
    m.name = "read";
    m.content = text;
    return m;
}

} // namespace

TEST(anthropic_flavor_and_endpoints) {
    auto d = anthropic();
    agent::Config cfg;
    cfg.api_base = "https://api.anthropic.com";
    ASSERT_EQ(d->flavor(), "anthropic");
    ASSERT_EQ(d->chat_url(cfg), "https://api.anthropic.com/v1/messages");
    ASSERT_EQ(d->models_url(cfg), "https://api.anthropic.com/v1/models");
}

TEST(anthropic_auth_headers) {
    auto d = anthropic();
    agent::Config cfg;

    // Version header is always present; the key header only with a key.
    auto headers = d->auth_headers(cfg);
    ASSERT_EQ(headers.size(), 1u);
    ASSERT_EQ(headers[0], "anthropic-version: 2023-06-01");

    cfg.api_key = "sk-ant-test";
    headers = d->auth_headers(cfg);
    ASSERT_EQ(headers.size(), 2u);
    ASSERT_EQ(headers[0], "x-api-key: sk-ant-test");
    ASSERT_EQ(headers[1], "anthropic-version: 2023-06-01");
}

// The internal model is OpenAI-shaped (one system block, tool messages with
// tool_call_id, tool_calls with JSON-string arguments). The Messages API wants
// one top-level `system` string, tool_result blocks, and tool_use blocks with
// parsed input objects.
TEST(anthropic_request_body_translates_internal_messages) {
    auto d = anthropic();
    agent::Config cfg;
    cfg.model = "claude-sonnet-4-5";
    cfg.max_tokens = 2048;

    agent::Message system_a = text_msg("system", "You are amber.");
    agent::Message system_b = text_msg("system", "Environment: darwin.");
    agent::Message user = text_msg("user", "read the file");

    agent::Message assistant;
    assistant.role = "assistant";
    assistant.content = "on it";
    assistant.tool_calls = agent::json::array({
        {{"id", "toolu_1"},
         {"type", "function"},
         {"function",
          {{"name", "read"}, {"arguments", R"({"path":"a.txt"})"}}}}});

    std::vector<agent::Message> msgs = {system_a, system_b, user, assistant,
                                        tool_result_msg("toolu_1", "file body")};
    agent::json body = d->build_chat_body(cfg, msgs, {}, true);

    ASSERT_EQ(body["model"], "claude-sonnet-4-5");
    ASSERT_EQ(body["max_tokens"], 2048);
    ASSERT_TRUE(body["stream"].get<bool>());
    // Both system messages merge into ONE top-level string.
    ASSERT_EQ(body["system"], "You are amber.\n\nEnvironment: darwin.");

    const agent::json& out = body["messages"];
    ASSERT_EQ(out.size(), 3u);

    ASSERT_EQ(out[0]["role"], "user");
    ASSERT_EQ(out[0]["content"][0]["type"], "text");
    ASSERT_EQ(out[0]["content"][0]["text"], "read the file");

    ASSERT_EQ(out[1]["role"], "assistant");
    ASSERT_EQ(out[1]["content"][0]["text"], "on it");
    ASSERT_EQ(out[1]["content"][1]["type"], "tool_use");
    ASSERT_EQ(out[1]["content"][1]["id"], "toolu_1");
    ASSERT_EQ(out[1]["content"][1]["name"], "read");
    // arguments (a JSON string internally) become a parsed input object.
    ASSERT_TRUE(out[1]["content"][1]["input"].is_object());
    ASSERT_EQ(out[1]["content"][1]["input"]["path"], "a.txt");

    ASSERT_EQ(out[2]["role"], "user");
    ASSERT_EQ(out[2]["content"][0]["type"], "tool_result");
    ASSERT_EQ(out[2]["content"][0]["tool_use_id"], "toolu_1");
    ASSERT_EQ(out[2]["content"][0]["content"], "file body");
}

TEST(anthropic_tools_use_input_schema) {
    auto d = anthropic();
    agent::Config cfg;
    std::shared_ptr<agent::Tool> tool = agent::make_bash_tool();
    std::vector<std::shared_ptr<agent::Tool>> tools = {tool};
    std::vector<agent::Message> msgs = {text_msg("user", "hi")};
    agent::json body = d->build_chat_body(cfg, msgs, tools, false);
    ASSERT_TRUE(body.contains("tools"));
    ASSERT_EQ(body["tools"].size(), 1u);
    ASSERT_EQ(body["tools"][0]["name"], "bash");
    // The Messages API calls the parameters schema `input_schema`, not
    // `parameters`, and has no `type: "function"` discriminator.
    ASSERT_TRUE(body["tools"][0].contains("input_schema"));
    ASSERT_FALSE(body["tools"][0].contains("parameters"));
    ASSERT_FALSE(body["tools"][0].contains("type"));
}

TEST(anthropic_parse_completion_maps_blocks_and_usage) {
    auto d = anthropic();
    const std::string raw = R"({
        "id": "msg_1",
        "type": "message",
        "role": "assistant",
        "content": [
            {"type": "text", "text": "Let me check."},
            {"type": "tool_use", "id": "toolu_9", "name": "read",
             "input": {"path": "b.txt"}}
        ],
        "usage": {"input_tokens": 120, "output_tokens": 42}
    })";

    agent::Message m = d->parse_completion(raw);
    ASSERT_EQ(m.role, "assistant");
    ASSERT_EQ(m.content, "Let me check.");
    ASSERT(m.tool_calls.is_array());
    ASSERT_EQ(m.tool_calls.size(), 1u);
    ASSERT_EQ(m.tool_calls[0]["function"]["name"], "read");
    // Tool arguments come back as a JSON string on the internal model.
    ASSERT_TRUE(m.tool_calls[0]["function"]["arguments"].is_string());
    agent::json args = agent::json::parse(
        m.tool_calls[0]["function"]["arguments"].get<std::string>(), nullptr,
        false);
    ASSERT_EQ(args["path"], "b.txt");

    agent::TokenUsage usage = d->parse_usage(raw);
    ASSERT_EQ(usage.prompt, 120L);
    ASSERT_EQ(usage.completion, 42L);
}

TEST(anthropic_parse_completion_malformed_degrades) {
    auto d = anthropic();
    agent::Message m = d->parse_completion("not json at all");
    ASSERT_EQ(m.role, "assistant");
    const std::string prefix = "[error: malformed LLM response, raw body follows]";
    ASSERT_EQ(m.content.compare(0, prefix.size(), prefix), 0);
}

TEST(anthropic_stream_decodes_named_events) {
    auto d = anthropic();
    agent::Message out;
    std::string streamed;
    auto decoder = d->make_decoder(
        out, [&streamed](const agent::StreamChunk& ch) {
            if (!ch.done) streamed += ch.delta;
        },
        "");

    const std::string sse =
        "event: message_start\n"
        "data: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":77}}}\n\n"
        "event: content_block_start\n"
        "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello \"}}\n\n"
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"world\"}}\n\n"
        "event: content_block_stop\n"
        "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
        "event: content_block_start\n"
        "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_7\",\"name\":\"search\"}}\n\n"
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"pattern\\\":\"}}\n\n"
        "event: content_block_delta\n"
        "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"\\\"ncurses\\\"}\"}}\n\n"
        "event: message_delta\n"
        "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"},\"usage\":{\"output_tokens\":9}}\n\n"
        "event: message_stop\n"
        "data: {\"type\":\"message_stop\"}\n\n";
    decoder->on_write(sse.c_str(), sse.size(), 1);
    decoder->finalize();

    ASSERT_EQ(out.content, "Hello world");
    ASSERT_EQ(streamed, "Hello world");
    ASSERT_EQ(decoder->prompt_tokens(), 77L);
    ASSERT_EQ(decoder->completion_tokens(), 9L);

    ASSERT(out.tool_calls.is_array());
    ASSERT_EQ(out.tool_calls.size(), 1u);
    ASSERT_EQ(out.tool_calls[0]["function"]["name"], "search");
    agent::json args = agent::json::parse(
        out.tool_calls[0]["function"]["arguments"].get<std::string>(), nullptr,
        false);
    ASSERT_FALSE(args.is_discarded());
    ASSERT_EQ(args["pattern"], "ncurses");
}

// A tool_use block whose name never arrived (truncated stream) must not
// survive: dispatch would deny an unknown tool and poison history.
TEST(anthropic_stream_drops_nameless_tool_blocks) {
    auto d = anthropic();
    agent::Message out;
    auto decoder = d->make_decoder(out, [](const agent::StreamChunk&) {}, "");
    const std::string sse =
        "event: content_block_start\n"
        "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_x\"}}\n\n"
        "event: nope\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{}\"}}\n\n";
    decoder->on_write(sse.c_str(), sse.size(), 1);
    decoder->finalize();
    ASSERT(out.tool_calls.is_null() || out.tool_calls.empty());
}

TEST(anthropic_model_list_and_probe_parse) {
    auto d = anthropic();
    const std::string body = R"({"data":[
        {"type":"model","id":"claude-sonnet-4-5","display_name":"Claude Sonnet 4.5"},
        {"type":"model","id":"claude-haiku-4-5","display_name":"Claude Haiku 4.5"}]})";

    auto models = d->parse_model_list_response(body);
    ASSERT_EQ(models.size(), 2u);
    ASSERT_EQ(models[0].id, "claude-sonnet-4-5");
    ASSERT_EQ(models[0].context, 0);   // the API does not report a window

    agent::ServerInfo info = d->parse_models_response(body, "claude-haiku-4-5");
    ASSERT_TRUE(info.ok);
    ASSERT_EQ(info.model, "claude-haiku-4-5");
}

TEST(anthropic_error_classification) {
    auto d = anthropic();
    ASSERT_TRUE(d->is_retryable(429, "{}"));       // rate limited
    ASSERT_TRUE(d->is_retryable(500, "{}"));
    ASSERT_TRUE(d->is_retryable(529, "{}"));       // overloaded
    ASSERT_FALSE(d->is_retryable(400, "{}"));      // bad request
    ASSERT_FALSE(d->is_retryable(401, "{}"));      // bad key

    // "prompt is too long: 213456 tokens > 200000 maximum"
    const std::string overflow =
        R"({"type":"error","error":{"type":"invalid_request_error","message":"prompt is too long: 213456 tokens > 200000 maximum"}})";
    ASSERT_EQ(d->context_overflow_hint(overflow), 200000);
    ASSERT_EQ(d->context_overflow_hint(R"({"error":"nope"})"), 0);
}

// Response bodies are untrusted input: a non-string field must degrade to
// empty/dropped, never throw — an exception inside the curl write callback
// cannot be recovered from.
TEST(anthropic_malformed_fields_do_not_throw) {
    auto d = anthropic();

    // Buffered: wrong-typed text/id/name blocks degrade; a nameless tool_use
    // is dropped rather than entering history.
    const std::string raw = R"({
        "content": [
            {"type": "text", "text": 12345},
            {"type": "tool_use", "id": 7, "name": 9, "input": "not-an-object"},
            {"type": 42, "text": "ignored"}
        ],
        "usage": {"input_tokens": "many", "output_tokens": null}
    })";
    agent::Message m = d->parse_completion(raw);
    ASSERT_EQ(m.role, "assistant");
    ASSERT_EQ(m.content, "");
    ASSERT(m.tool_calls.is_null() || m.tool_calls.empty());

    agent::TokenUsage usage = d->parse_usage(raw);
    ASSERT_EQ(usage.prompt, -1L);
    ASSERT_EQ(usage.completion, -1L);

    // Streamed: wrong-typed event fields are dropped, not exceptions.
    agent::Message out;
    auto decoder = d->make_decoder(out, [](const agent::StreamChunk&) {}, "");
    const std::string sse =
        "data: {\"type\": 5}\n\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":77}}\n\n"
        "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"tool_use\",\"id\":1,\"name\":2}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":9}}\n\n"
        "data: {\"type\":\"message_delta\",\"usage\":{\"output_tokens\":\"x\"}}\n\n"
        "data: [DONE]\n\n";
    decoder->on_write(sse.c_str(), sse.size(), 1);
    decoder->finalize();
    ASSERT_EQ(out.content, "");
    ASSERT(out.tool_calls.is_null() || out.tool_calls.empty());
}

// The registry resolves the flavor to the right dialect and falls back for
// unknown ones — the single dispatch point for the whole provider layer.
TEST(dialect_registry_resolves_and_falls_back) {
    // openai is the one protocol the core itself provides; every vendor
    // protocol is contributed by its provider plugin.
    ASSERT_EQ(agent::make_dialect("openai")->flavor(), "openai");

    // The Messages API dialect ships with the anthropic plugin. With the
    // plugin not registered (as here) its flavor is simply unknown, which is
    // the typo case: fall back rather than refuse.
    ASSERT_EQ(agent::make_dialect("anthropic")->flavor(), "openai");
    ASSERT_EQ(agent::make_dialect("no-such-flavor")->flavor(), "openai");

    // Registering it (what the plugin does on activation) makes the flavor
    // resolve, and switching the plugin off makes it refuse loudly instead.
    agent::register_dialect("anthropic",
                            [] { return agent::make_anthropic_dialect(); },
                            "anthropic");
    ASSERT_EQ(agent::make_dialect("anthropic")->flavor(), "anthropic");
    ASSERT_TRUE(agent::flavor_unavailable_reason("anthropic").empty());

    ASSERT_TRUE(agent::unregister_dialect("anthropic", "anthropic"));
    const std::string reason = agent::flavor_unavailable_reason("anthropic");
    ASSERT_FALSE(reason.empty());
    ASSERT(reason.find("/set plugin anthropic on") != std::string::npos);
}

TEST(dialect_registry_accepts_new_factories) {
    agent::register_dialect("testflavor", []() {
        return agent::make_anthropic_dialect();   // any Dialect works
    });
    ASSERT_EQ(agent::make_dialect("testflavor")->flavor(), "anthropic");
}
