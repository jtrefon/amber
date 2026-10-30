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
