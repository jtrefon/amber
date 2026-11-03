// Prompt caching over the real transport.
//
// The dialect tests pin the JSON shape. These drive HttpLLMClient against a
// local endpoint that applies the provider's cache rules, so what is asserted
// is the behaviour a real account would see: a breakpoint that survives turns,
// a prefix that is read instead of re-prefilled, and the counter split the
// context gauge depends on. A unit test on build_chat_body cannot catch a
// transport that drops a field or a usage parser that reports only the
// uncached tail.

#include "agent/dialect_anthropic.h"
#include "agent/llm.h"
#include "mock_anthropic.h"
#include "test_util.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace {

using json = nlohmann::json;

agent::Config mock_config(const agent::test::MockAnthropic& mock) {
    agent::Config cfg;
    cfg.api_base = mock.base();
    cfg.api_key = "test-key";
    cfg.model = "claude-sonnet-4-5";
    cfg.flavor = "anthropic";
    return cfg;
}

agent::Message text_msg(const std::string& role, const std::string& content) {
    agent::Message m;
    m.role = role;
    m.content = content;
    return m;
}

// A stable prompt long enough to clear the mock's default minimum.
std::string stable_prompt() {
    return std::string(8000, 's');
}

const std::string* header(const agent::test::MockRequest& req, const char* name) {
    auto it = req.headers.find(name);
    return it == req.headers.end() ? nullptr : &it->second;
}

} // namespace

TEST(anthropic_wire_marks_the_stable_prefix) {
    agent::test::MockAnthropic mock;
    ASSERT(mock.start());
    agent::HttpLLMClient client(mock_config(mock), agent::make_anthropic_dialect());

    std::vector<agent::Message> msgs{text_msg("system", "BASE PROMPT"), text_msg("user", "hi")};
    agent::Stats stats;
    client.chat(msgs, {}, &stats);

    const auto reqs = mock.requests();
    ASSERT_EQ(reqs.size(), 1u);
    ASSERT_EQ(reqs[0].path, "/v1/messages");
    const std::string* version = header(reqs[0], "anthropic-version");
    ASSERT(version && *version == "2023-06-01");
    const std::string* key = header(reqs[0], "x-api-key");
    ASSERT(key && *key == "test-key");
    // The request identifies the harness, not libcurl.
    const std::string* ua = header(reqs[0], "user-agent");
    ASSERT(ua && ua->rfind("amber/", 0) == 0);

    json body = json::parse(reqs[0].body, nullptr, false);
    ASSERT_FALSE(body.is_discarded());
    ASSERT(body["system"].is_array());
    ASSERT(body["system"][0].contains("cache_control"));
    ASSERT_EQ(body["system"][0]["cache_control"]["type"].get<std::string>(), "ephemeral");
    // The moving breakpoint: the newest turn is marked so the next request can
    // read the conversation, not only the prompt.
    ASSERT(body["messages"].back()["content"].back().contains("cache_control"));
}

TEST(anthropic_wire_reuses_the_prefix_across_turns) {
    agent::test::MockAnthropic mock;
    ASSERT(mock.start());
    agent::HttpLLMClient client(mock_config(mock), agent::make_anthropic_dialect());

    std::vector<agent::Message> msgs{text_msg("system", stable_prompt()),
                                     text_msg("user", "first")};
    agent::Stats first;
    client.chat(msgs, {}, &first);

    msgs.push_back(text_msg("assistant", "reply"));
    msgs.push_back(text_msg("user", "second"));
    agent::Stats second;
    client.chat(msgs, {}, &second);

    ASSERT_EQ(first.cache_read_tokens, 0L);
    ASSERT(first.cache_write_tokens > 0);
    ASSERT(second.cache_read_tokens > 0);

    // The provider reports input_tokens as the part AFTER the last breakpoint.
    // Presenting that as the prompt size makes the context gauge and the
    // compression gate believe a cached conversation is nearly empty.
    const agent::test::MockUsage usage = mock.last_usage();
    ASSERT_EQ(second.prompt_tokens, usage.input + usage.read + usage.write);
    ASSERT(second.prompt_tokens > second.cache_read_tokens);
}

TEST(anthropic_stream_reuses_the_prefix_across_turns) {
    agent::test::MockAnthropic mock;
    ASSERT(mock.start());
    agent::HttpLLMClient client(mock_config(mock), agent::make_anthropic_dialect());

    std::vector<agent::Message> msgs{text_msg("system", stable_prompt()),
                                     text_msg("user", "first")};
    agent::Stats first;
    client.chat_stream(msgs, {}, [](const agent::StreamChunk&) {}, &first);

    msgs.push_back(text_msg("assistant", "reply"));
    msgs.push_back(text_msg("user", "second"));
    agent::Stats second;
    client.chat_stream(msgs, {}, [](const agent::StreamChunk&) {}, &second);

    ASSERT(second.cache_read_tokens > 0);
    const agent::test::MockUsage usage = mock.last_usage();
    ASSERT_EQ(second.prompt_tokens, usage.input + usage.read + usage.write);
}

// The tail blocks (session brief, activated skills) ride the user channel after
// the conversation. When one of them changes, the stable prompt must still be
// read: only the conversation behind the change is re-prefilled.
TEST(anthropic_tail_change_keeps_the_stable_prefix_cached) {
    agent::test::MockAnthropic mock;
    ASSERT(mock.start());
    agent::HttpLLMClient client(mock_config(mock), agent::make_anthropic_dialect());

    std::vector<agent::Message> msgs{text_msg("system", stable_prompt()),
                                     text_msg("user", "first")};
    agent::Stats warm;
    client.chat(msgs, {}, &warm);

    msgs.push_back(text_msg("assistant", "reply"));
    msgs.push_back(text_msg("user", "second"));
    agent::Message brief = text_msg("user", "brief v1");
    brief.harness = true;
    msgs.push_back(brief);
    agent::Stats before;
    client.chat(msgs, {}, &before);

    msgs.back().content = "brief v2";
    agent::Stats after;
    client.chat(msgs, {}, &after);

    ASSERT(after.cache_read_tokens > 0);
    // Only the changed tail is re-written; the stable prompt is read.
    ASSERT(after.cache_read_tokens > after.cache_write_tokens);
}

// Below the model's minimum the provider caches nothing and returns no error:
// both counters are 0 and the request looks exactly like a client that sent no
// breakpoint at all. This is the state the user sees as "unknown" in a
// provider dashboard, so the harness has to be able to name it.
TEST(anthropic_below_the_minimum_reports_no_cache) {
    agent::test::MockAnthropic mock;
    mock.set_min_cacheable_tokens(1000000);
    ASSERT(mock.start());
    agent::HttpLLMClient client(mock_config(mock), agent::make_anthropic_dialect());

    std::vector<agent::Message> msgs{text_msg("system", stable_prompt()),
                                     text_msg("user", "first")};
    agent::Stats first;
    client.chat(msgs, {}, &first);
    msgs.push_back(text_msg("assistant", "reply"));
    msgs.push_back(text_msg("user", "second"));
    agent::Stats second;
    client.chat(msgs, {}, &second);

    ASSERT_EQ(first.cache_write_tokens, 0L);
    ASSERT_EQ(second.cache_read_tokens, 0L);
    ASSERT_EQ(second.cache_write_tokens, 0L);
    const agent::test::MockUsage usage = mock.last_usage();
    ASSERT_EQ(second.prompt_tokens, usage.input);
}

// The diagnostics opt-in, driven through the real client: the first request
// serializes `previous_message_id: null`, and every later request replays the
// id the provider returned for the one before it. Without that round-trip the
// provider has no baseline to compare and can only report "no divergence",
// which is indistinguishable from a healthy cache.
TEST(anthropic_client_replays_the_previous_response_id) {
    agent::test::MockAnthropic mock;
    ASSERT_TRUE(mock.start());

    agent::Config cfg = mock_config(mock);
    cfg.cache_diagnostics = true;
    agent::HttpLLMClient client(cfg, agent::make_anthropic_dialect());

    std::vector<agent::Message> messages = {text_msg("system", "You are amber."),
                                            text_msg("user", stable_prompt())};
    agent::Stats first_stats;
    client.chat(messages, {}, &first_stats);

    messages.push_back(text_msg("assistant", "done"));
    messages.push_back(text_msg("user", "again"));
    agent::Stats second_stats;
    client.chat(messages, {}, &second_stats);

    const auto requests = mock.requests();
    ASSERT(requests.size() >= 2u);
    const agent::json first = agent::json::parse(requests[0].body);
    const agent::json second = agent::json::parse(requests[1].body);
    ASSERT(first.contains("diagnostics"));
    ASSERT(first["diagnostics"]["previous_message_id"].is_null());
    ASSERT_EQ(second["diagnostics"]["previous_message_id"].get<std::string>(),
              std::string("msg_mock"));
    ASSERT_EQ(second_stats.response_id, std::string("msg_mock"));
}

// The streamed path must carry the response id too: diagnostics replay it on
// the next turn, and a streaming session never sees a buffered body.
TEST(anthropic_stream_captures_the_response_id) {
    agent::test::MockAnthropic mock;
    ASSERT(mock.start());
    agent::Config cfg = mock_config(mock);
    cfg.cache_diagnostics = true;
    agent::HttpLLMClient client(cfg, agent::make_anthropic_dialect());

    std::vector<agent::Message> msgs{text_msg("system", stable_prompt()),
                                     text_msg("user", "first")};
    agent::Stats stats;
    client.chat_stream(msgs, {}, [](const agent::StreamChunk&) {}, &stats);
    ASSERT_EQ(stats.response_id, std::string("msg_mock"));
}

// The provider's miss reason travels with the streamed response too, and the
// second streamed request replays the id the first one returned.
TEST(anthropic_stream_reports_the_miss_reason_and_replays_the_id) {
    agent::test::MockAnthropic mock;
    ASSERT(mock.start());
    mock.set_cache_miss_reason("system_changed");
    agent::Config cfg = mock_config(mock);
    cfg.cache_diagnostics = true;
    agent::HttpLLMClient client(cfg, agent::make_anthropic_dialect());

    std::vector<agent::Message> msgs{text_msg("system", stable_prompt()),
                                     text_msg("user", "first")};
    agent::Stats first;
    client.chat_stream(msgs, {}, [](const agent::StreamChunk&) {}, &first);
    ASSERT_EQ(first.cache_miss_reason, std::string("system_changed"));

    msgs.push_back(text_msg("assistant", "reply"));
    msgs.push_back(text_msg("user", "second"));
    agent::Stats second;
    client.chat_stream(msgs, {}, [](const agent::StreamChunk&) {}, &second);

    const auto requests = mock.requests();
    ASSERT(requests.size() >= 2u);
    const agent::json body = agent::json::parse(requests[1].body);
    ASSERT_EQ(body["diagnostics"]["previous_message_id"].get<std::string>(),
              std::string("msg_mock"));
}

// A provider that rejects `cache_control.scope` must not take the session down.
// A real gateway answered 400 with
// "messages.256.content.0.tool_result.cache_control.ephemeral.scope: extra
// inputs are not permitted"; the client retries once without the field, keeps
// the breakpoints, remembers for the session, and leaves a notice.
TEST(anthropic_client_degrades_when_scope_is_rejected) {
    agent::test::MockAnthropic mock;
    ASSERT(mock.start());
    mock.reject_once("\"scope\":\"global\"",
                     "messages.0.content.0.tool_result.cache_control.ephemeral.scope: extra "
                     "inputs are not permitted");
    agent::Config cfg = mock_config(mock);
    cfg.cache_global_scope = true;
    agent::HttpLLMClient client(cfg, agent::make_anthropic_dialect());

    std::vector<agent::Message> msgs{text_msg("system", stable_prompt()),
                                     text_msg("user", "first")};
    agent::Stats stats;
    client.chat(msgs, {}, &stats);
    ASSERT(stats.valid);

    const auto requests = mock.requests();
    ASSERT(requests.size() >= 2u);
    ASSERT(requests[0].body.find("\"scope\":\"global\"") != std::string::npos);
    ASSERT(requests[1].body.find("\"scope\":\"global\"") == std::string::npos);
    // The breakpoints survive: only the scope field is dropped.
    ASSERT(requests[1].body.find("cache_control") != std::string::npos);
    const auto notices = client.take_notices();
    ASSERT(!notices.empty());
}
