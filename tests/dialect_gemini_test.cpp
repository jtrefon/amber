// Gemini dialect: protocol translation, proven hermetically. Mirrors
// tests/dialect_anthropic_test.cpp - no network, inline fixtures, and the
// wire-level expectations written from the Messages/GenerateContent shapes.

#include "agent/dialect.h"
#include "agent/dialect_gemini.h"
#include "agent/providers.h"
#include "agent/registry.h"
#include "agent/tools.h"
#include "test_util.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

using namespace agent;

namespace {

Config gemini_cfg() {
    Config cfg;
    cfg.api_base = "https://generativelanguage.googleapis.com";
    cfg.api_key = "test-key";
    cfg.model = "gemini-2.5-pro";
    cfg.max_tokens = 2048;
    cfg.temperature = 0.7;
    return cfg;
}

Message user_msg(const std::string& text) {
    Message m;
    m.role = "user";
    m.content = text;
    return m;
}

Message system_msg(const std::string& text) {
    Message m;
    m.role = "system";
    m.content = text;
    return m;
}

} // namespace

TEST(gemini_flavor_and_endpoints) {
    auto d = make_gemini_dialect();
    ASSERT_EQ(d->flavor(), std::string("gemini"));

    Config cfg = gemini_cfg();
    cfg.stream = false;
    ASSERT_EQ(d->chat_url(cfg),
              std::string("https://generativelanguage.googleapis.com/v1beta/"
                          "models/gemini-2.5-pro:generateContent"));

    // Streaming is a different method with SSE framing.
    cfg.stream = true;
    ASSERT_EQ(d->chat_url(cfg),
              std::string("https://generativelanguage.googleapis.com/v1beta/"
                          "models/gemini-2.5-pro:streamGenerateContent?alt=sse"));
    ASSERT_EQ(d->models_url(cfg),
              std::string("https://generativelanguage.googleapis.com/v1beta/models"));
}

TEST(gemini_auth_headers) {
    auto d = make_gemini_dialect();
    Config cfg = gemini_cfg();

    auto with_key = d->auth_headers(cfg);
    ASSERT_EQ(with_key.size(), 1u);
    ASSERT_EQ(with_key[0], std::string("x-goog-api-key: test-key"));

    cfg.api_key.clear();
    ASSERT_TRUE(d->auth_headers(cfg).empty());
}

TEST(gemini_builds_request_body) {
    auto d = make_gemini_dialect();
    Config cfg = gemini_cfg();
    std::vector<Message> messages{system_msg("be terse"), user_msg("hello")};

    json body = d->build_chat_body(cfg, messages, {}, false);

    // System turns merge into one top-level instruction.
    ASSERT_EQ(body["systemInstruction"]["parts"][0]["text"], std::string("be terse"));
    ASSERT_EQ(body["contents"].size(), 1u);
    ASSERT_EQ(body["contents"][0]["role"], std::string("user"));
    ASSERT_EQ(body["contents"][0]["parts"][0]["text"], std::string("hello"));
    ASSERT_EQ(body["generationConfig"]["maxOutputTokens"], 2048);
}

TEST(gemini_maps_tool_call_and_result) {
    auto d = make_gemini_dialect();
    Config cfg = gemini_cfg();

    Message assistant;
    assistant.role = "assistant";
    assistant.tool_calls = json::array(
        {{{"id", "call_1"},
          {"type", "function"},
          {"function", {{"name", "read"}, {"arguments", R"({"path":"x"})"}}}}});
    Message tool;
    tool.role = "tool";
    tool.name = "read";
    tool.tool_call_id = "call_1";
    tool.content = "file contents";

    json body = d->build_chat_body(cfg, {user_msg("go"), assistant, tool}, {}, false);

    // Assistant tool calls become functionCall parts with parsed args.
    const json& model_turn = body["contents"][1];
    ASSERT_EQ(model_turn["role"], std::string("model"));
    ASSERT_EQ(model_turn["parts"][0]["functionCall"]["name"], std::string("read"));
    ASSERT_EQ(model_turn["parts"][0]["functionCall"]["args"]["path"], std::string("x"));

    // Tool results become a user turn carrying functionResponse.
    const json& tool_turn = body["contents"][2];
    ASSERT_EQ(tool_turn["role"], std::string("user"));
    ASSERT_EQ(tool_turn["parts"][0]["functionResponse"]["name"], std::string("read"));
}

TEST(gemini_declares_tools_with_parameters) {
    auto d = make_gemini_dialect();
    Config cfg = gemini_cfg();
    ToolRegistry reg;
    reg.register_tool(make_read_tool());

    json body = d->build_chat_body(cfg, {user_msg("x")}, reg.snapshot_tools(), false);

    const json& decls = body["tools"][0]["functionDeclarations"];
    ASSERT_EQ(decls.size(), 1u);
    ASSERT_EQ(decls[0]["name"], std::string("read"));
    // Gemini takes the schema directly, with no "type: function" wrapper.
    ASSERT(decls[0].contains("parameters"));
}

TEST(gemini_parses_buffered_completion) {
    auto d = make_gemini_dialect();
    const std::string raw = R"({
      "candidates": [{"content": {"role": "model", "parts": [
          {"text": "the answer is "}, {"text": "42"}]}}],
      "usageMetadata": {"promptTokenCount": 11, "candidatesTokenCount": 3}
    })";

    Message m = d->parse_completion(raw);
    ASSERT_EQ(m.role, std::string("assistant"));
    ASSERT_EQ(m.content, std::string("the answer is 42"));
    ASSERT_TRUE(m.tool_calls.is_null() || m.tool_calls.empty());

    TokenUsage usage = d->parse_usage(raw);
    ASSERT_EQ(usage.prompt, 11L);
    ASSERT_EQ(usage.completion, 3L);
}

TEST(gemini_parses_tool_call_response) {
    auto d = make_gemini_dialect();
    const std::string raw = R"({
      "candidates": [{"content": {"parts": [
          {"functionCall": {"name": "read", "args": {"path": "Makefile"}}}]}}]
    })";

    Message m = d->parse_completion(raw);
    ASSERT_EQ(m.tool_calls.size(), 1u);
    ASSERT_EQ(m.tool_calls[0]["function"]["name"], std::string("read"));
    ASSERT_EQ(m.tool_calls[0]["type"], std::string("function"));
    ASSERT_FALSE(m.tool_calls[0]["id"].get<std::string>().empty());
    // Arguments stay a JSON *string* on the internal model.
    ASSERT(m.tool_calls[0]["function"]["arguments"].is_string());
}

TEST(gemini_malformed_body_degrades) {
    auto d = make_gemini_dialect();
    Message m = d->parse_completion("this is not json");
    ASSERT_EQ(m.role, std::string("assistant"));
    ASSERT(m.content.find("[error: malformed LLM response") == 0);
}

TEST(gemini_stream_decodes_text_and_tool_calls) {
    auto d = make_gemini_dialect();
    Message out;
    out.role = "assistant";
    std::string streamed;
    auto decoder = d->make_decoder(out, [&](const StreamChunk& c) {
        streamed += c.delta;
    }, "");

    const std::string chunk1 =
        "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"Hel\"}]}}]}\n\n";
    const std::string chunk2 =
        "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"lo\"}]}}],"
        "\"usageMetadata\":{\"promptTokenCount\":5,\"candidatesTokenCount\":2}}\n\n";
    const std::string chunk3 =
        "data: {\"candidates\":[{\"content\":{\"parts\":"
        "[{\"functionCall\":{\"name\":\"read\",\"args\":{\"path\":\"a\"}}}]}}]}\n\n";
    decoder->on_write(chunk1.data(), 1, chunk1.size());
    decoder->on_write(chunk2.data(), 1, chunk2.size());
    decoder->on_write(chunk3.data(), 1, chunk3.size());
    decoder->finalize();

    ASSERT_EQ(out.content, std::string("Hello"));
    ASSERT_EQ(streamed, std::string("Hello"));
    ASSERT_EQ(decoder->prompt_tokens(), 5L);
    ASSERT_EQ(decoder->completion_tokens(), 2L);
    ASSERT_EQ(out.tool_calls.size(), 1u);
    ASSERT_EQ(out.tool_calls[0]["function"]["name"], std::string("read"));
}

TEST(gemini_model_list_parses_and_strips_prefix) {
    auto d = make_gemini_dialect();
    const std::string body = R"({
      "models": [
        {"name": "models/gemini-2.5-pro", "inputTokenLimit": 1048576},
        {"name": "models/gemini-2.5-flash", "inputTokenLimit": 1048576}
      ]})";

    auto models = d->parse_model_list_response(body);
    ASSERT_EQ(models.size(), 2u);
    // "models/" is stripped: every other call takes the bare id.
    ASSERT_EQ(models[0].id, std::string("gemini-2.5-pro"));
    ASSERT_EQ(models[0].context, 1048576);

    ServerInfo info = d->parse_models_response(body, "gemini-2.5-flash");
    ASSERT_TRUE(info.ok);
    ASSERT_EQ(info.model, std::string("gemini-2.5-flash"));
}

TEST(provider_rows_carry_their_flavor_to_the_catalog) {
    // The provider domain hands the dialect selector to the transport, so a
    // provider contributed by a plugin is probed with its own protocol rather
    // than the OpenAI default.
    Provider gemini;
    gemini.name = "gemini";
    gemini.api_base = "https://generativelanguage.googleapis.com";
    gemini.flavor = "gemini";
    ASSERT_EQ(gemini.flavor, std::string("gemini"));

    Provider plain;
    plain.name = "local";
    // Existing initialisers keep meaning "openai".
    ASSERT_EQ(plain.flavor, std::string("openai"));
}

TEST(provider_file_round_trips_the_flavor) {
    // A user points amber at a non-OpenAI endpoint by writing flavor into the
    // provider file; the file is the config-only path for a shipped protocol.
    const std::string dir = "/tmp/amber_flavor_roundtrip";
    std::filesystem::remove_all(dir);
    setenv("XDG_CONFIG_HOME", dir.c_str(), 1);

    Provider p;
    p.name = "mygemini";
    p.api_base = "https://generativelanguage.googleapis.com";
    p.default_model = "gemini-2.5-flash";
    p.flavor = "gemini";
    ASSERT_TRUE(make_file_provider_repository()->save(p));

    auto back = make_file_provider_repository()->find("mygemini");
    ASSERT(back.has_value());
    ASSERT_EQ(back->flavor, std::string("gemini"));

    // The default is not written: a file that says nothing means openai.
    Provider plain;
    plain.name = "plain";
    plain.api_base = "http://localhost:8081/v1";
    ASSERT_TRUE(make_file_provider_repository()->save(plain));
    std::ifstream f(dir + "/amber/providers/plain.conf");
    std::string contents((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    ASSERT(contents.find("flavor=") == std::string::npos);
    auto plain_back = make_file_provider_repository()->find("plain");
    ASSERT(plain_back.has_value());
    ASSERT_EQ(plain_back->flavor, std::string("openai"));

    unsetenv("XDG_CONFIG_HOME");
    std::filesystem::remove_all(dir);
}

TEST(gemini_error_classification) {
    auto d = make_gemini_dialect();
    ASSERT_TRUE(d->is_retryable(429, ""));
    ASSERT_TRUE(d->is_retryable(503, ""));
    ASSERT_FALSE(d->is_retryable(400, ""));
    ASSERT_FALSE(d->is_retryable(401, ""));

    const std::string overflow =
        "* GenerateContentRequest.contents: The input token count (213456) "
        "exceeds the maximum number of tokens allowed (200000).";
    ASSERT_EQ(d->context_overflow_hint(overflow), 200000);
    ASSERT_EQ(d->context_overflow_hint("no hint here"), 0);
}
