
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
    ASSERT_EQ(d->models_url(cfg), "https://api.anthropic.com/v1/models?limit=1000");
}

// The endpoint a user pastes from the docs already carries the version
// ("https://api.anthropic.com/v1" is what every example shows), and the edit
// form strips only trailing slashes. Appending the version unconditionally
// would request /v1/v1/models -- a 404 that no config file can explain.
TEST(anthropic_endpoints_tolerate_a_versioned_base) {
    auto d = anthropic();
    agent::Config cfg;
    cfg.api_base = "https://api.anthropic.com/v1";
    ASSERT_EQ(d->chat_url(cfg), "https://api.anthropic.com/v1/messages");
    ASSERT_EQ(d->models_url(cfg), "https://api.anthropic.com/v1/models?limit=1000");

    // A trailing slash is the same class of paste; the form trims it, a
    // hand-edited file need not.
    cfg.api_base = "https://api.anthropic.com/";
    ASSERT_EQ(d->chat_url(cfg), "https://api.anthropic.com/v1/messages");
    ASSERT_EQ(d->models_url(cfg), "https://api.anthropic.com/v1/models?limit=1000");

    // A path that merely ENDS in "v1" is not a version segment.
    cfg.api_base = "https://proxy.example/anthropic/v1";
    ASSERT_EQ(d->models_url(cfg), "https://proxy.example/anthropic/v1/models?limit=1000");
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
    assistant.tool_calls = agent::json::array(
        {{{"id", "toolu_1"},
          {"type", "function"},
          {"function", {{"name", "read"}, {"arguments", R"({"path":"a.txt"})"}}}}});

    std::vector<agent::Message> msgs = {system_a, system_b, user, assistant,
                                        tool_result_msg("toolu_1", "file body")};
    agent::json body = d->build_chat_body(cfg, msgs, {}, true);

    ASSERT_EQ(body["model"], "claude-sonnet-4-5");
    ASSERT_EQ(body["max_tokens"], 2048);
    ASSERT_TRUE(body["stream"].get<bool>());
    // The protocol's top-level system field is a block array once caching is
    // on: the stable prompt carries the breakpoint, the injected block follows
    // it so a change there cannot invalidate the cached prefix.
    ASSERT(body["system"].is_array());
    ASSERT_EQ(body["system"].size(), 2u);
    ASSERT_EQ(body["system"][0]["text"], "You are amber.");
    ASSERT_EQ(body["system"][0]["cache_control"]["type"], "ephemeral");
    ASSERT_EQ(body["system"][1]["text"], "Environment: darwin.");
    ASSERT(!body["system"][1].contains("cache_control"));

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

// Anthropic rejects an EMPTY text block -- "text content block must be
// non-empty" -- where OpenAI tolerates "". The builder used to emit one for
// every message with no text (a cancelled turn, a reply that carried only
// tool_use blocks, an empty assistant message already in history), which 400s
// the whole request and then every later request in that session.
TEST(anthropic_request_omits_empty_text_blocks) {
    auto d = anthropic();
    agent::Config cfg;
    cfg.model = "claude-sonnet-4-5";

    std::vector<agent::Message> msgs = {text_msg("user", "hi"),
                                        text_msg("assistant", ""), // e.g. a cancelled turn
                                        text_msg("user", "still there?")};
    agent::json body = d->build_chat_body(cfg, msgs, {}, true);

    const agent::json& out = body["messages"];
    ASSERT_EQ(out.size(), 2u); // the empty turn is skipped, not sent
    for (const auto& m : out) {
        ASSERT_FALSE(m["content"].empty()); // no empty content arrays either
        for (const auto& b : m["content"]) {
            if (b["type"] == "text")
                ASSERT_FALSE(b["text"].get<std::string>().empty());
        }
    }
    ASSERT_EQ(out[0]["content"][0]["text"], "hi");
    ASSERT_EQ(out[1]["content"][0]["text"], "still there?");

    // A tool-only assistant turn keeps its blocks (there is no text to drop),
    // and a history that stops mid-tool gets its tool_use closed immediately
    // after: the pairing is the API's contract, so the unanswered id is
    // answered explicitly rather than left to 400 the next request.
    agent::Message tool_only;
    tool_only.role = "assistant";
    tool_only.tool_calls = agent::json::array(
        {{{"id", "toolu_2"},
          {"type", "function"},
          {"function", {{"name", "read"}, {"arguments", R"({"path":"b.txt"})"}}}}});
    std::vector<agent::Message> with_tool = {text_msg("user", "read it"), tool_only};
    agent::json body2 = d->build_chat_body(cfg, with_tool, {}, true);
    ASSERT_EQ(body2["messages"].size(), 3u);
    ASSERT_EQ(body2["messages"][1]["content"].size(), 1u);
    ASSERT_EQ(body2["messages"][1]["content"][0]["type"], "tool_use");
    ASSERT_EQ(body2["messages"][2]["content"][0]["type"], "tool_result");
    ASSERT_EQ(body2["messages"][2]["content"][0]["tool_use_id"], "toolu_2");
}

// Anthropic requires EVERY tool_result of a turn inside the ONE message that
// immediately follows the assistant's tool_use blocks. The internal history
// keeps one message per result, so a two-tool turn used to go out as
// assistant -> user(result A) -> user(result B), and the API answered
// "tool_use ids were found without tool_result blocks immediately after",
// naming B. A result that never arrived (a cancelled dispatch, a crash mid
// tool-run) is answered in that same message too, because the pairing is the
// API's contract and an unanswered id 400s every later request in the session.
TEST(anthropic_tool_results_share_the_message_that_follows) {
    auto d = anthropic();
    agent::Config cfg;
    cfg.model = "claude-sonnet-4-5";

    agent::Message assistant;
    assistant.role = "assistant";
    assistant.content = "reading both";
    assistant.tool_calls = agent::json::array();
    assistant.tool_calls.push_back(
        {{"id", "call_a"},
         {"type", "function"},
         {"function", {{"name", "read"}, {"arguments", R"({"path":"a"})"}}}});
    assistant.tool_calls.push_back(
        {{"id", "call_b"},
         {"type", "function"},
         {"function", {{"name", "read"}, {"arguments", R"({"path":"b"})"}}}});

    std::vector<agent::Message> msgs = {text_msg("user", "read both"), assistant,
                                        tool_result_msg("call_a", "A body"),
                                        tool_result_msg("call_b", "B body")};
    agent::json body = d->build_chat_body(cfg, msgs, {}, true);
    const agent::json& out = body["messages"];
    ASSERT_EQ(out.size(), 3u); // user, assistant, ONE user carrying both results
    ASSERT_EQ(out[2]["role"], "user");
    ASSERT_EQ(out[2]["content"].size(), 2u);
    ASSERT_EQ(out[2]["content"][0]["tool_use_id"], "call_a");
    ASSERT_EQ(out[2]["content"][1]["tool_use_id"], "call_b");

    // A result that never arrived is answered explicitly, in that same
    // immediately-following message.
    std::vector<agent::Message> partial = {text_msg("user", "go"), assistant,
                                           tool_result_msg("call_a", "A body")};
    agent::json body2 = d->build_chat_body(cfg, partial, {}, true);
    const agent::json& out2 = body2["messages"];
    ASSERT_EQ(out2.size(), 3u);
    ASSERT_EQ(out2[2]["content"].size(), 2u);
    ASSERT_EQ(out2[2]["content"][1]["tool_use_id"], "call_b");
    ASSERT_FALSE(out2[2]["content"][1]["content"].get<std::string>().empty());

    // An empty result reads as a missing block to the API; say so instead.
    std::vector<agent::Message> empty = {text_msg("user", "go"), assistant,
                                         tool_result_msg("call_a", ""),
                                         tool_result_msg("call_b", "B body")};
    agent::json body3 = d->build_chat_body(cfg, empty, {}, true);
    ASSERT_FALSE(body3["messages"][2]["content"][0]["content"].get<std::string>().empty());
}

// An assistant tool turn whose ids were never answered (a cancelled or failed
// dispatch, a session saved mid-turn) must close in a user message of its own
// BEFORE the next assistant turn. Emitting those answers inside the next
// assistant message leaves the ids unanswered -- "tool_use ids were found
// without tool_result blocks immediately after" -- and puts a tool_result
// block in a role the API has no place for, which 400s every later request in
// the session.
TEST(anthropic_unanswered_turn_closes_before_the_next_assistant_message) {
    auto d = anthropic();
    agent::Config cfg;
    cfg.model = "claude-sonnet-4-5";

    agent::Message abandoned;
    abandoned.role = "assistant";
    abandoned.tool_calls = agent::json::array();
    abandoned.tool_calls.push_back(
        {{"id", "call_1"},
         {"type", "function"},
         {"function", {{"name", "read"}, {"arguments", R"({"path":"a"})"}}}});

    agent::Message next;
    next.role = "assistant";
    next.tool_calls = agent::json::array();
    next.tool_calls.push_back({{"id", "call_2"},
                               {"type", "function"},
                               {"function", {{"name", "read"}, {"arguments", R"({"path":"b"})"}}}});

    std::vector<agent::Message> msgs = {text_msg("user", "go"), abandoned, next,
                                        tool_result_msg("call_2", "B body")};
    agent::json body = d->build_chat_body(cfg, msgs, {}, true);

    const agent::json& out = body["messages"];
    ASSERT_EQ(out.size(), 5u); // user, abandoned turn, answers, next turn, result
    ASSERT_EQ(out[1]["role"], "assistant");
    ASSERT_EQ(out[1]["content"][0]["type"], "tool_use");
    ASSERT_EQ(out[2]["role"], "user");
    ASSERT_EQ(out[2]["content"][0]["type"], "tool_result");
    ASSERT_EQ(out[2]["content"][0]["tool_use_id"], "call_1");
    ASSERT_FALSE(out[2]["content"][0]["content"].get<std::string>().empty());
    ASSERT_EQ(out[3]["role"], "assistant");
    ASSERT_EQ(out[3]["content"][0]["id"], "call_2");
    ASSERT_EQ(out[4]["role"], "user");
    ASSERT_EQ(out[4]["content"][0]["tool_use_id"], "call_2");

    // A tool_result block has no valid place in an assistant message.
    for (const auto& m : out)
        if (m["role"] == "assistant")
            for (const auto& b : m["content"])
                ASSERT_FALSE(b["type"] == "tool_result");
}

// A call that reached history without an id -- a text-extracted <tool_call>
// from a template-style model, or a router that omits ids -- still needs one
// on the wire: every tool_use must carry a non-empty id and its tool_result
// must echo it. The id is numbered from the history alone, so replaying the
// same conversation sends the same ids.
TEST(anthropic_wire_synthesizes_missing_tool_ids) {
    auto d = anthropic();
    agent::Config cfg;
    cfg.model = "claude-sonnet-4-5";

    agent::Message reply;
    reply.role = "assistant";
    reply.tool_calls = agent::json::array();
    reply.tool_calls.push_back(
        {{"type", "function"}, {"function", {{"name", "read"}, {"arguments", R"({"path":"a"})"}}}});
    reply.tool_calls.push_back(
        {{"id", ""},
         {"type", "function"},
         {"function", {{"name", "read"}, {"arguments", R"({"path":"b"})"}}}});

    std::vector<agent::Message> msgs = {text_msg("user", "go"), reply,
                                        tool_result_msg("", "A body"),
                                        tool_result_msg("", "B body")};
    agent::json body = d->build_chat_body(cfg, msgs, {}, true);

    const agent::json& out = body["messages"];
    ASSERT_EQ(out.size(), 3u);
    ASSERT_EQ(out[1]["content"].size(), 2u);
    const auto id_a = out[1]["content"][0]["id"].get<std::string>();
    const auto id_b = out[1]["content"][1]["id"].get<std::string>();
    ASSERT_FALSE(id_a.empty());
    ASSERT_FALSE(id_b.empty());
    ASSERT(id_a != id_b);
    ASSERT_EQ(out[2]["role"], "user");
    ASSERT_EQ(out[2]["content"][0]["tool_use_id"], id_a);
    ASSERT_EQ(out[2]["content"][1]["tool_use_id"], id_b);
}

// A tool_result the turn never asked for has no valid place in the protocol
// ("unexpected tool_use_id found in tool_result blocks"); it is dropped rather
// than replayed.
TEST(anthropic_wire_drops_tool_results_nobody_asked_for) {
    auto d = anthropic();
    agent::Config cfg;
    cfg.model = "claude-sonnet-4-5";

    agent::Message reply;
    reply.role = "assistant";
    reply.tool_calls = agent::json::array();
    reply.tool_calls.push_back(
        {{"id", "call_1"},
         {"type", "function"},
         {"function", {{"name", "read"}, {"arguments", R"({"path":"a"})"}}}});

    std::vector<agent::Message> msgs = {text_msg("user", "go"), reply,
                                        tool_result_msg("call_1", "A body"),
                                        tool_result_msg("call_orphan", "stray")};
    agent::json body = d->build_chat_body(cfg, msgs, {}, true);

    const agent::json& out = body["messages"];
    ASSERT_EQ(out.size(), 3u);
    ASSERT_EQ(out[2]["content"].size(), 1u);
    ASSERT_EQ(out[2]["content"][0]["tool_use_id"], "call_1");
    for (const auto& m : out)
        for (const auto& b : m["content"])
            ASSERT_FALSE(b.value("tool_use_id", std::string()) == "call_orphan");
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
        m.tool_calls[0]["function"]["arguments"].get<std::string>(), nullptr, false);
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
        out,
        [&streamed](const agent::StreamChunk& ch) {
            if (!ch.done)
                streamed += ch.delta;
        },
        "");

    const std::string sse =
        "event: message_start\n"
        "data: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":77}}}\n\n"
        "event: content_block_start\n"
        "data: "
        "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\","
        "\"text\":\"\"}}\n\n"
        "event: content_block_delta\n"
        "data: "
        "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":"
        "\"Hello \"}}\n\n"
        "event: content_block_delta\n"
        "data: "
        "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":"
        "\"world\"}}\n\n"
        "event: content_block_stop\n"
        "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
        "event: content_block_start\n"
        "data: "
        "{\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"tool_use\","
        "\"id\":\"toolu_7\",\"name\":\"search\"}}\n\n"
        "event: content_block_delta\n"
        "data: "
        "{\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"input_json_delta\","
        "\"partial_json\":\"{\\\"pattern\\\":\"}}\n\n"
        "event: content_block_delta\n"
        "data: "
        "{\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"input_json_delta\","
        "\"partial_json\":\"\\\"ncurses\\\"}\"}}\n\n"
        "event: message_delta\n"
        "data: "
        "{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"},\"usage\":{\"output_"
        "tokens\":9}}\n\n"
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
        out.tool_calls[0]["function"]["arguments"].get<std::string>(), nullptr, false);
    ASSERT_FALSE(args.is_discarded());
    ASSERT_EQ(args["pattern"], "ncurses");
}

// The streamed usage split has the same contract as the buffered one: the
// provider reports input_tokens as what sits AFTER the last breakpoint, so a
// cache hit makes it tiny. The context gauge reads this number, and taking it
// at face value showed "4 tokens used" on a conversation of thousands.
TEST(anthropic_stream_prompt_tokens_include_the_cached_prefix) {
    auto d = anthropic();
    agent::Message out;
    auto decoder = d->make_decoder(out, [](const agent::StreamChunk&) {}, "");

    const std::string sse =
        "event: message_start\n"
        "data: "
        "{\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":4,"
        "\"cache_read_input_tokens\":8000,\"cache_creation_input_tokens\":12}}}\n\n"
        "event: message_stop\n"
        "data: {\"type\":\"message_stop\"}\n\n";
    decoder->on_write(sse.c_str(), sse.size(), 1);
    decoder->finalize();

    ASSERT_EQ(decoder->prompt_tokens(), 8016L);
    ASSERT_EQ(decoder->cache_read_tokens(), 8000L);
    ASSERT_EQ(decoder->cache_write_tokens(), 12L);
}

// A tool_use block whose name never arrived (truncated stream) must not
// survive: dispatch would deny an unknown tool and poison history.
TEST(anthropic_stream_drops_nameless_tool_blocks) {
    auto d = anthropic();
    agent::Message out;
    auto decoder = d->make_decoder(out, [](const agent::StreamChunk&) {}, "");
    const std::string sse = "event: content_block_start\n"
                            "data: "
                            "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{"
                            "\"type\":\"tool_use\",\"id\":\"toolu_x\"}}\n\n"
                            "event: nope\n"
                            "data: "
                            "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":"
                            "\"input_json_delta\",\"partial_json\":\"{}\"}}\n\n";
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
    // A known Claude family falls back to its documented constants when the
    // proxy omits the listing fields; this is not a guess for an unknown id.
    ASSERT_EQ(models[0].context, 200000);
    ASSERT_EQ(models[0].max_output, 64000);
    ASSERT_TRUE(models[0].reasoning);

    agent::ServerInfo info = d->parse_models_response(body, "claude-haiku-4-5");
    ASSERT_TRUE(info.ok);
    ASSERT_EQ(info.model, "claude-haiku-4-5");
    ASSERT_EQ(info.context_size, 200000);
}

// The listing reports max_input_tokens (the model's input window), so the
// context gauge and the compression budget can be sized from it instead of
// staying "unknown" until a request overflows.
TEST(anthropic_model_list_reports_the_input_window) {
    auto d = anthropic();
    const std::string body = R"({"data":[
        {"type":"model","id":"claude-sonnet-4-5","max_input_tokens":200000},
        {"type":"model","id":"claude-haiku-4-5","max_input_tokens":null}]})";

    auto models = d->parse_model_list_response(body);
    ASSERT_EQ(models.size(), 2u);
    ASSERT_EQ(models[0].context, 200000);
    // null falls back to the known Claude family; it is not read as 0 tokens.
    ASSERT_EQ(models[1].context, 200000);
    ASSERT_EQ(models[1].max_output, 64000);

    agent::ServerInfo info = d->parse_models_response(body, "claude-sonnet-4-5");
    ASSERT_TRUE(info.ok);
    ASSERT_EQ(info.context_size, 200000);
}

TEST(anthropic_known_family_fallback_is_bounded) {
    auto d = anthropic();
    const std::string body = R"({"data":[
        {"type":"model","id":"claude-sonnet-4-5","max_output_tokens":32000},
        {"type":"model","id":"claude-experimental"},
        {"type":"model","id":"gpt-4"}]})";

    auto models = d->parse_model_list_response(body);
    ASSERT_EQ(models.size(), 3u);
    // The documented family supplies the context; a reported output limit wins.
    ASSERT_EQ(models[0].context, 200000);
    ASSERT_EQ(models[0].max_output, 32000);
    ASSERT_TRUE(models[0].reasoning);
    // An unknown id is not guessed.
    ASSERT_EQ(models[1].context, 0);
    ASSERT_EQ(models[1].max_output, 0);
    ASSERT_FALSE(models[1].reasoning);
    ASSERT_EQ(models[2].context, 0);
}

TEST(anthropic_request_respects_output_and_reasoning) {
    auto d = anthropic();
    agent::Config cfg;
    cfg.model = "claude-sonnet-4-5";
    cfg.max_tokens = 16384;
    cfg.model_max_output = 8192;
    cfg.model_supports_reasoning = true;
    cfg.model_thinking_legacy = true;
    cfg.reasoning_effort = "high";
    std::vector<agent::Message> msgs;

    auto body = d->build_chat_body(cfg, msgs, {}, false);
    ASSERT_EQ(body["max_tokens"].get<int>(), 8192); // never above the model's ceiling
    ASSERT(body.contains("thinking"));
    ASSERT_EQ(body["thinking"]["type"], "enabled");
    ASSERT_EQ(body["thinking"]["budget_tokens"].get<int>(), 4096); // numeric budget, not a tag

    cfg.max_tokens = 2048; // the budget must stay below max_tokens
    body = d->build_chat_body(cfg, msgs, {}, false);
    ASSERT_EQ(body["thinking"]["budget_tokens"].get<int>(), 2047);

    // A model whose listing said nothing about thinking gets no block, even
    // when a previous model advertised reasoning: unknown is not a guess.
    cfg.model_supports_reasoning = false;
    cfg.model_thinking_legacy = false;
    cfg.max_tokens = 16384;
    body = d->build_chat_body(cfg, msgs, {}, false);
    ASSERT(!body.contains("thinking"));
}

// The Models API publishes the output ceiling as `max_tokens` (not
// `max_output_tokens`), publishes the input window as `max_input_tokens`, and
// says which thinking types it accepts. All of it must reach the capabilities
// the request and the picker read.
TEST(anthropic_model_list_reads_limits_capabilities_and_efforts) {
    auto d = anthropic();
    const std::string body = R"({"data":[
        {"type":"model","id":"claude-sonnet-5","display_name":"Claude Sonnet 5",
         "max_input_tokens":1000000,"max_tokens":128000,"line":"sonnet",
         "capabilities":{"thinking":{"types":{
            "disabled":{"supported":true},
            "between_tools":{"supported":true}}}}},
        {"type":"model","id":"claude-opus-5-5","max_input_tokens":1000000,"max_tokens":128000,
         "capabilities":{"thinking":{"types":{"disabled":{"supported":false}}}}},
        {"type":"model","id":"claude-haiku-4-5","max_output_tokens":32000},
        {"type":"model","id":"claude-3-5-sonnet","max_tokens":8192}]})";

    auto models = d->parse_model_list_response(body);
    ASSERT_EQ(models.size(), 4u);

    ASSERT_EQ(models[0].context, 1000000);
    ASSERT_EQ(models[0].max_output, 128000);
    ASSERT_TRUE(models[0].thinking_adaptive);
    ASSERT_FALSE(models[0].thinking_legacy);
    ASSERT_FALSE(models[0].thinking_always_on);
    ASSERT_TRUE(models[0].thinking_disabled_ok);
    ASSERT_TRUE(models[0].thinking_between_tools_ok);
    ASSERT_EQ(models[0].line, "sonnet");
    ASSERT_EQ(models[0].default_effort, "high");
    ASSERT_EQ(models[0].efforts.size(), 5u); // low, medium, high, xhigh, max

    // The 5.5 Opus is always-on; the listing's disabled:false wins over the
    // family default.
    ASSERT_TRUE(models[1].thinking_adaptive);
    ASSERT_TRUE(models[1].thinking_always_on);
    ASSERT_FALSE(models[1].thinking_disabled_ok);
    ASSERT_EQ(models[1].default_effort, "medium");

    // Old spelling still works when a gateway sends it.
    ASSERT_EQ(models[2].max_output, 32000);
    ASSERT_EQ(models[2].context, 200000);
    ASSERT_TRUE(models[2].thinking_legacy);

    // A pre-reasoning family stays non-reasoning.
    ASSERT_FALSE(models[3].reasoning);
    ASSERT_FALSE(models[3].thinking_adaptive);
}

// 4.6+ takes adaptive thinking plus output_config.effort. Sending a budget
// there, or an effort the model does not list, is a 400.
TEST(anthropic_request_uses_adaptive_thinking_and_effort) {
    auto d = anthropic();
    agent::Config cfg;
    cfg.model = "claude-sonnet-5";
    cfg.max_tokens = 16384;
    cfg.model_supports_reasoning = true;
    cfg.model_thinking_adaptive = true;
    cfg.model_thinking_disabled_ok = true;
    cfg.model_efforts = {"low", "medium", "high", "xhigh", "max"};
    cfg.model_default_effort = "high";
    cfg.reasoning_effort = "xhigh";
    std::vector<agent::Message> msgs;

    auto body = d->build_chat_body(cfg, msgs, {}, false);
    ASSERT_EQ(body["thinking"]["type"], "adaptive");
    ASSERT(!body["thinking"].contains("budget_tokens"));
    ASSERT_EQ(body["output_config"]["effort"], "xhigh");
    ASSERT(!body.contains("temperature"));

    // The model does not list xhigh (4.6): send its documented default, not a
    // level the API would reject.
    cfg.model_efforts = {"low", "medium", "high", "max"};
    cfg.model_default_effort = "high";
    body = d->build_chat_body(cfg, msgs, {}, false);
    ASSERT_EQ(body["output_config"]["effort"], "high");

    // Off on a model that accepts disabled.
    cfg.reasoning_effort = "off";
    body = d->build_chat_body(cfg, msgs, {}, false);
    ASSERT_EQ(body["thinking"]["type"], "disabled");
    ASSERT(!body.contains("output_config"));
}

// An always-on model cannot be disabled. between_tools keeps the model's
// default reasoning but stops it running between tool calls; when neither type
// is advertised, the block is omitted rather than sent and rejected.
TEST(anthropic_request_off_on_always_on_model_never_sends_disabled) {
    auto d = anthropic();
    agent::Config cfg;
    cfg.model = "claude-opus-5-5";
    cfg.max_tokens = 16384;
    cfg.model_supports_reasoning = true;
    cfg.model_thinking_adaptive = true;
    cfg.model_thinking_always_on = true;
    cfg.reasoning_effort = "off";
    std::vector<agent::Message> msgs;

    cfg.model_thinking_between_tools_ok = true;
    auto body = d->build_chat_body(cfg, msgs, {}, false);
    ASSERT_EQ(body["thinking"]["type"], "between_tools");

    cfg.model_thinking_between_tools_ok = false;
    body = d->build_chat_body(cfg, msgs, {}, false);
    ASSERT(!body.contains("thinking"));
}

// Opus 4.5 accepts both the legacy budget and an effort. Sonnet/Haiku 4.5 have
// no effort list, so they get a budget only.
TEST(anthropic_request_legacy_budget_and_effort) {
    auto d = anthropic();
    agent::Config cfg;
    cfg.model = "claude-opus-4-5";
    cfg.max_tokens = 16384;
    cfg.model_supports_reasoning = true;
    cfg.model_thinking_legacy = true;
    cfg.model_efforts = {"low", "medium", "high"};
    cfg.model_default_effort = "high";
    cfg.reasoning_effort = "high";
    std::vector<agent::Message> msgs;

    auto body = d->build_chat_body(cfg, msgs, {}, false);
    ASSERT_EQ(body["thinking"]["type"], "enabled");
    ASSERT_EQ(body["thinking"]["budget_tokens"].get<int>(), 4096);
    ASSERT_EQ(body["output_config"]["effort"], "high");

    // An unsupported level clamps to the default instead of reaching the API.
    cfg.reasoning_effort = "max";
    body = d->build_chat_body(cfg, msgs, {}, false);
    ASSERT_EQ(body["output_config"]["effort"], "high");
    ASSERT_EQ(body["thinking"]["budget_tokens"].get<int>(), 4096);

    // No effort list: budget from the configured level, no effort block.
    cfg.model_efforts.clear();
    cfg.reasoning_effort = "medium";
    body = d->build_chat_body(cfg, msgs, {}, false);
    ASSERT_EQ(body["thinking"]["budget_tokens"].get<int>(), 2048);
    ASSERT(!body.contains("output_config"));
}

// The API requires thinking blocks (with their signatures) back verbatim on the
// next turn. Rebuilding the assistant message from text + tool_calls dropped
// them, which is a 400 on the tool loop for every reasoning model.
TEST(anthropic_thinking_blocks_survive_the_tool_round_trip) {
    auto d = anthropic();
    const std::string raw = R"({
        "content": [
            {"type":"thinking","thinking":"I should read it.","signature":"sig_abc"},
            {"type":"text","text":"on it"},
            {"type":"tool_use","id":"toolu_1","name":"read","input":{"path":"a.txt"}}
        ],
        "usage": {"input_tokens": 10, "output_tokens": 5}
    })";
    agent::Message assistant = d->parse_completion(raw);
    ASSERT_EQ(assistant.tool_calls.size(), 1u);
    ASSERT(assistant.raw_content.is_array());
    ASSERT_EQ(assistant.raw_content.size(), 3u);
    ASSERT_EQ(assistant.raw_content[0]["signature"], "sig_abc");

    std::vector<agent::Message> msgs = {text_msg("user", "read a.txt"), assistant,
                                        tool_result_msg("toolu_1", "file body")};
    agent::json body = d->build_chat_body(agent::Config{}, msgs, {}, false);
    const agent::json& sent = body["messages"];
    ASSERT_EQ(sent.size(), 3u);
    ASSERT_EQ(sent[1]["role"], "assistant");
    ASSERT_EQ(sent[1]["content"].size(), 3u);
    ASSERT_EQ(sent[1]["content"][0]["type"], "thinking");
    ASSERT_EQ(sent[1]["content"][0]["signature"], "sig_abc");
    ASSERT_EQ(sent[1]["content"][2]["type"], "tool_use");
    ASSERT_EQ(sent[1]["content"][2]["input"]["path"], "a.txt");

    // The streamed path keeps the same blocks: a signature arrives as its own
    // delta and input as fragments.
    agent::Message out;
    auto decoder = d->make_decoder(out, [](const agent::StreamChunk&) {}, "");
    const std::string sse =
        "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":"
        "{\"type\":\"thinking\",\"thinking\":\"\"}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":"
        "{\"type\":\"thinking_delta\",\"thinking\":\"plan\"}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":"
        "{\"type\":\"signature_delta\",\"signature\":\"sig_xyz\"}}\n\n"
        "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":"
        "{\"type\":\"tool_use\",\"id\":\"toolu_2\",\"name\":\"read\"}}\n\n"
        "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":"
        "{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"path\\\":\\\"b.txt\\\"}\"}}\n\n";
    decoder->on_write(sse.c_str(), sse.size(), 1);
    decoder->finalize();
    ASSERT(out.raw_content.is_array());
    ASSERT_EQ(out.raw_content.size(), 2u);
    ASSERT_EQ(out.raw_content[0]["thinking"], "plan");
    ASSERT_EQ(out.raw_content[0]["signature"], "sig_xyz");
    ASSERT_EQ(out.raw_content[1]["input"]["path"], "b.txt");
}

TEST(anthropic_error_classification) {
    auto d = anthropic();
    ASSERT_TRUE(d->is_retryable(429, "{}")); // rate limited
    ASSERT_TRUE(d->is_retryable(500, "{}"));
    ASSERT_TRUE(d->is_retryable(529, "{}"));  // overloaded
    ASSERT_FALSE(d->is_retryable(400, "{}")); // bad request
    ASSERT_FALSE(d->is_retryable(401, "{}")); // bad key

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
        "data: "
        "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":"
        "77}}\n\n"
        "data: "
        "{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"tool_use\","
        "\"id\":1,\"name\":2}}\n\n"
        "data: "
        "{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_json_delta\","
        "\"partial_json\":9}}\n\n"
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
    agent::register_dialect(
        "anthropic", [] { return agent::make_anthropic_dialect(); }, "anthropic");
    ASSERT_EQ(agent::make_dialect("anthropic")->flavor(), "anthropic");
    ASSERT_TRUE(agent::flavor_unavailable_reason("anthropic").empty());

    ASSERT_TRUE(agent::unregister_dialect("anthropic", "anthropic"));
    const std::string reason = agent::flavor_unavailable_reason("anthropic");
    ASSERT_FALSE(reason.empty());
    ASSERT(reason.find("/set plugin anthropic on") != std::string::npos);
}

TEST(dialect_registry_accepts_new_factories) {
    agent::register_dialect("testflavor", []() {
        return agent::make_anthropic_dialect(); // any Dialect works
    });
    ASSERT_EQ(agent::make_dialect("testflavor")->flavor(), "anthropic");
}
