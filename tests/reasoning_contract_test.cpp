// The reasoning contract is one catalogue value, carried from a provider's
// /models body through ServerInfo into Config, and consumed by the request
// builder and the UI. These tests pin the four shapes amber has to understand:
// OpenRouter's supported_parameters + top-level reasoning, DeepSeek's effort
// object, OpenCode's models.dev capabilities.reasoning, and the documented
// OpenAI family fallback. They also pin the echo rule: a reasoning model that
// requires its chain of thought back on a tool turn gets it.

#include "agent/dialect.h"
#include "agent/llm.h"
#include "test_util.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {

using agent::Config;
using agent::Message;
using agent::ModelInfo;
using agent::ReasoningOff;
using agent::ReasoningWire;

std::unique_ptr<agent::Dialect> openai() {
    return agent::make_dialect("openai");
}

std::vector<ModelInfo> parse(const std::string& body) {
    return openai()->parse_model_list_response(body);
}

ModelInfo only(const std::vector<ModelInfo>& models) {
    ASSERT_EQ(models.size(), 1u);
    return models[0];
}

Config config_for(const ModelInfo& m) {
    Config cfg;
    cfg.model = m.id;
    agent::apply_model_capabilities(cfg, agent::capabilities_of(m));
    return cfg;
}

const agent::json& assistant_message(const agent::json& body) {
    for (const auto& msg : body["messages"])
        if (msg.value("role", "") == "assistant")
            return msg;
    agent::test::fail("no assistant message in request body");
}

agent::json tool_call() {
    return agent::json::array(
        {{{"id", "call_1"},
          {"type", "function"},
          {"function", {{"name", "read"}, {"arguments", R"({"path":"x"})"}}}}});
}

constexpr const char* kDeepSeek = R"({"data":[{
  "id":"deepseek-flash",
  "context_window":1048576,
  "max_output_tokens":393216,
  "input_modalities":["text","image"],
  "effort":{"supported_levels":["low","high","max"],"default_level":"high"}
}]})";

constexpr const char* kOpenRouter = R"({"data":[{
  "id":"google/gemini-4-hydrogen",
  "context_length":1048576,
  "top_provider":{"max_completion_tokens":65536},
  "supported_parameters":["reasoning","reasoning_effort","tools"],
  "reasoning":{
    "supported_efforts":["xhigh","high","medium","low"],
    "default_effort":"medium",
    "mandatory":true
  }
}]})";

constexpr const char* kOpenCode = R"({"data":[{
  "id":"deepseek-v4.1-flash",
  "limit":{"context":1000000,"output":384000},
  "capabilities":{"reasoning":{"supported":true,"toggle":true,"effort":["low","high","max"]}},
  "api":{"chat-completions":{"compatibility":{"reasoning_field":"reasoning_content"}}}
}]})";

} // namespace

TEST(openai_model_list_reads_deepseek_effort_metadata) {
    const auto& m = only(parse(kDeepSeek));
    ASSERT(m.reasoning.supported);
    ASSERT_EQ(m.reasoning.efforts.size(), 3u);
    ASSERT_EQ(m.reasoning.default_effort, std::string("high"));
    ASSERT(m.reasoning.wire == ReasoningWire::Effort);
    ASSERT(m.reasoning.effort_levels);
    ASSERT(m.reasoning.off == ReasoningOff::ThinkingDisabled);
    ASSERT(m.reasoning.echo_field == std::string("reasoning_content"));
    ASSERT(m.reasoning.require_echo);
    ASSERT(std::find(m.input_modalities.begin(), m.input_modalities.end(), "image") !=
           m.input_modalities.end());
}

TEST(openai_model_list_reads_openrouter_reasoning_object) {
    const auto& m = only(parse(kOpenRouter));
    ASSERT(m.reasoning.supported);
    ASSERT_EQ(m.reasoning.efforts.size(), 4u);
    ASSERT_EQ(m.reasoning.default_effort, std::string("medium"));
    ASSERT(m.reasoning.mandatory);
    ASSERT(m.reasoning.wire == ReasoningWire::ReasoningObject);
    ASSERT(m.reasoning.effort_levels);
    ASSERT(m.reasoning.off == ReasoningOff::Unsupported);
    ASSERT(m.reasoning.echo_details);
    ASSERT_FALSE(m.reasoning.require_echo); // only tool turns that carry it
}

TEST(openai_model_list_reads_opencode_reasoning_capabilities) {
    const auto& m = only(parse(kOpenCode));
    ASSERT(m.reasoning.supported);
    ASSERT_EQ(m.reasoning.efforts.size(), 3u);
    ASSERT_FALSE(m.reasoning.mandatory);
    ASSERT(m.reasoning.off == ReasoningOff::EffortNone);
    ASSERT(m.reasoning.effort_levels);
    ASSERT(m.reasoning.wire == ReasoningWire::Effort);
    ASSERT(m.reasoning.echo_field == std::string("reasoning_content"));
    ASSERT(m.reasoning.require_echo);
}

TEST(openai_model_list_reads_context_and_output_aliases) {
    const auto& deepseek = only(parse(kDeepSeek));
    ASSERT_EQ(deepseek.context, 1048576);
    ASSERT_EQ(deepseek.context_train, 1048576);
    ASSERT_EQ(deepseek.max_output, 393216);

    const auto& opencode = only(parse(kOpenCode));
    ASSERT_EQ(opencode.context, 1000000);
    ASSERT_EQ(opencode.context_train, 1000000);
    ASSERT_EQ(opencode.max_output, 384000);
}

TEST(openai_model_list_reads_openrouter_null_efforts_as_unclamped) {
    const auto& m = only(parse(R"({"data":[{"id":"openrouter/model",)"
                               R"("supported_parameters":["reasoning"],)"
                               R"("reasoning":{"supported_efforts":null,"mandatory":false}}]})"));
    ASSERT(m.reasoning.supported);
    ASSERT(m.reasoning.effort_levels);
    ASSERT(m.reasoning.efforts.empty());

    Config cfg = config_for(m);
    cfg.reasoning_effort = "max";
    auto body = openai()->build_chat_body(cfg, {}, {}, false);
    ASSERT_EQ(body["reasoning"]["effort"].get<std::string>(), "max");
    cfg.reasoning_effort = "off";
    body = openai()->build_chat_body(cfg, {}, {}, false);
    ASSERT_EQ(body["reasoning"]["enabled"].get<bool>(), false);
}

TEST(openai_model_list_reads_opencode_default_and_optional_echo) {
    const auto& m =
        only(parse(R"({"data":[{"id":"opencode/model",)"
                   R"("capabilities":{"reasoning":{"supported":true,"toggle":true,)"
                   R"("effort":["low","high"],"default":{"type":"effort","value":"high"}}},)"
                   R"("api":{"chat-completions":{"compatibility":{)"
                   R"("reasoning_field":"reasoning","require_reasoning":false}}}}]})"));
    ASSERT(m.reasoning.supported);
    ASSERT_EQ(m.reasoning.default_effort, std::string("high"));
    ASSERT_EQ(m.reasoning.echo_field, std::string("reasoning"));
    ASSERT_FALSE(m.reasoning.require_echo);

    Config cfg = config_for(m);
    cfg.reasoning_effort = "xhigh"; // unlisted: clamp to the listing default
    const auto body = openai()->build_chat_body(cfg, {}, {}, false);
    ASSERT_EQ(body["reasoning_effort"].get<std::string>(), "high");
}

TEST(openai_request_sends_deepseek_reasoning_contract) {
    const Config cfg = config_for(only(parse(kDeepSeek)));
    Config high = cfg;
    high.reasoning_effort = "high";
    auto body = openai()->build_chat_body(high, {}, {}, false);
    ASSERT_EQ(body["reasoning_effort"].get<std::string>(), "high");

    Config max = cfg;
    max.reasoning_effort = "max";
    body = openai()->build_chat_body(max, {}, {}, false);
    ASSERT_EQ(body["reasoning_effort"].get<std::string>(), "max");

    // An unlisted level is clamped to the listing's default, not sent as-is.
    Config medium = cfg;
    medium.reasoning_effort = "medium";
    body = openai()->build_chat_body(medium, {}, {}, false);
    ASSERT_EQ(body["reasoning_effort"].get<std::string>(), "high");

    Config off = cfg;
    off.reasoning_effort = "off";
    body = openai()->build_chat_body(off, {}, {}, false);
    ASSERT_EQ(body["thinking"]["type"].get<std::string>(), "disabled");
    ASSERT(!body.contains("reasoning_effort"));
}

TEST(openai_request_sends_openrouter_reasoning_object) {
    const Config cfg = config_for(only(parse(kOpenRouter)));
    Config high = cfg;
    high.reasoning_effort = "high";
    auto body = openai()->build_chat_body(high, {}, {}, false);
    ASSERT_EQ(body["reasoning"]["effort"].get<std::string>(), "high");
    ASSERT(!body.contains("reasoning_effort"));

    // A mandatory model cannot be disabled: off sends no field at all.
    Config off = cfg;
    off.reasoning_effort = "off";
    body = openai()->build_chat_body(off, {}, {}, false);
    ASSERT(!body.contains("reasoning"));
    ASSERT(!body.contains("reasoning_effort"));
    ASSERT(!body.contains("thinking"));
}

TEST(openai_request_sends_opencode_effort_and_none) {
    const Config cfg = config_for(only(parse(kOpenCode)));
    Config low = cfg;
    low.reasoning_effort = "low";
    auto body = openai()->build_chat_body(low, {}, {}, false);
    ASSERT_EQ(body["reasoning_effort"].get<std::string>(), "low");

    Config off = cfg;
    off.reasoning_effort = "off";
    body = openai()->build_chat_body(off, {}, {}, false);
    ASSERT_EQ(body["reasoning_effort"].get<std::string>(), "none");

    // xhigh is not one of this model's levels and it names no default, so the
    // request omits the field rather than inventing a level.
    Config xhigh = cfg;
    xhigh.reasoning_effort = "xhigh";
    body = openai()->build_chat_body(xhigh, {}, {}, false);
    ASSERT(!body.contains("reasoning_effort"));
    ASSERT(!body.contains("reasoning"));
}

TEST(openai_model_without_effort_controls_gets_no_level_field) {
    const auto& m = only(parse(R"({"data":[{"id":"big-pickle",)"
                               R"("capabilities":{"reasoning":{"supported":true}}}]})"));
    ASSERT(m.reasoning.supported);
    ASSERT_FALSE(m.reasoning.effort_levels);
    ASSERT(m.reasoning.mandatory); // no toggle, no "none" level

    Config cfg = config_for(m);
    cfg.reasoning_effort = "high";
    const auto body = openai()->build_chat_body(cfg, {}, {}, false);
    ASSERT(!body.contains("reasoning_effort"));
    ASSERT(!body.contains("reasoning"));
    ASSERT_FALSE(agent::reasoning_effort_allowed(cfg, "high"));
}

TEST(openai_request_sends_openai_family_fallback) {
    const auto& m = only(parse(R"({"data":[{"id":"gpt-5.1"}]})"));
    ASSERT(m.reasoning.supported);
    ASSERT(m.reasoning.mandatory);
    ASSERT(m.reasoning.effort_levels);
    ASSERT(m.reasoning.wire == ReasoningWire::Effort);

    Config cfg = config_for(m);
    cfg.reasoning_effort = "high";
    auto body = openai()->build_chat_body(cfg, {}, {}, false);
    ASSERT_EQ(body["reasoning_effort"].get<std::string>(), "high");

    cfg.reasoning_effort = "off";
    body = openai()->build_chat_body(cfg, {}, {}, false);
    ASSERT(!body.contains("reasoning_effort"));
}

TEST(openai_server_info_keeps_the_whole_reasoning_contract) {
    const std::string body = kOpenRouter;
    const auto info = openai()->parse_models_response(body, "google/gemini-4-hydrogen");
    ASSERT(info.reasoning.supported);
    ASSERT_EQ(info.reasoning.efforts.size(), 4u);
    ASSERT_EQ(info.reasoning.default_effort, std::string("medium"));
    ASSERT(info.reasoning.mandatory);

    Config cfg;
    agent::merge_server_info(cfg, info);
    ASSERT(cfg.model_reasoning.supported);
    ASSERT_EQ(cfg.model_reasoning.efforts.size(), 4u);
    ASSERT_EQ(cfg.model_reasoning.default_effort, std::string("medium"));
    ASSERT(cfg.model_reasoning.mandatory);
}

TEST(openai_completion_captures_reasoning_details) {
    const std::string body =
        R"({"choices":[{"message":{"role":"assistant","content":"done",)"
        R"("reasoning_details":[{"type":"reasoning.text","text":"think first",)"
        R"("format":"anthropic-claude-v1","id":"r1"}]}}]})";
    const Message m = openai()->parse_completion(body);
    ASSERT_EQ(m.content, std::string("done"));
    ASSERT(m.reasoning_details.is_array());
    ASSERT_EQ(m.reasoning_details[0]["text"].get<std::string>(), std::string("think first"));
}

TEST(openai_completion_drops_invalid_tool_arguments) {
    const std::string body = R"({"choices":[{"message":{"role":"assistant","content":"",)"
                             R"("tool_calls":[{"id":"call_1","type":"function",)"
                             R"("function":{"name":"read","arguments":"{not json"}}]}}]})";
    const Message m = openai()->parse_completion(body);
    ASSERT(m.tool_calls.is_null());
}

TEST(openai_body_replays_reasoning_content_on_tool_turns) {
    Config cfg;
    cfg.model_reasoning.supported = true;
    cfg.model_reasoning.echo_field = "reasoning_content";
    cfg.model_reasoning.require_echo = true;

    Message assistant;
    assistant.role = "assistant";
    assistant.reasoning = "think first";
    assistant.tool_calls = tool_call();
    Message result;
    result.role = "tool";
    result.tool_call_id = "call_1";
    result.name = "read";
    result.content = "ok";

    const auto body = openai()->build_chat_body(cfg, {assistant, result}, {}, false);
    ASSERT_EQ(assistant_message(body)["reasoning_content"].get<std::string>(), "think first");
}

TEST(openai_body_replays_reasoning_details_on_tool_turns) {
    Config cfg;
    cfg.model_reasoning.supported = true;
    cfg.model_reasoning.echo_field = "reasoning";
    cfg.model_reasoning.echo_details = true;
    cfg.model_reasoning.require_echo = true;

    Message assistant;
    assistant.role = "assistant";
    assistant.reasoning = "think first";
    assistant.reasoning_details = agent::json::array(
        {{{"type", "reasoning.text"}, {"text", "think first"}, {"format", "anthropic-claude-v1"}}});
    assistant.tool_calls = tool_call();

    const auto body = openai()->build_chat_body(cfg, {assistant}, {}, false);
    const auto& msg = assistant_message(body);
    ASSERT(msg.contains("reasoning_details"));
    ASSERT_EQ(msg["reasoning_details"][0]["text"].get<std::string>(), "think first");
}

TEST(openai_body_replays_plain_reasoning_when_details_are_absent) {
    Config cfg;
    cfg.model_reasoning.supported = true;
    cfg.model_reasoning.echo_field = "reasoning";
    cfg.model_reasoning.echo_details = true;
    cfg.model_reasoning.require_echo = true;

    Message assistant;
    assistant.role = "assistant";
    assistant.reasoning = "think first";
    assistant.tool_calls = tool_call();

    const auto body = openai()->build_chat_body(cfg, {assistant}, {}, false);
    const auto& msg = assistant_message(body);
    ASSERT(!msg.contains("reasoning_details"));
    ASSERT_EQ(msg["reasoning"].get<std::string>(), "think first");
}

TEST(openai_body_replays_the_response_field_without_a_catalogue) {
    // A cold catalogue must not resurrect the DeepSeek tool-turn 400: the
    // assistant turn remembers the field it arrived in.
    Config cfg;
    Message assistant;
    assistant.role = "assistant";
    assistant.reasoning = "think first";
    assistant.reasoning_field = "reasoning_content";
    assistant.tool_calls = tool_call();

    const auto body = openai()->build_chat_body(cfg, {assistant}, {}, false);
    ASSERT_EQ(assistant_message(body)["reasoning_content"].get<std::string>(), "think first");
}

TEST(openai_body_replays_reasoning_details_without_a_catalogue) {
    Config cfg;
    Message assistant;
    assistant.role = "assistant";
    assistant.reasoning_details =
        agent::json::array({{{"type", "reasoning.text"}, {"text", "think first"}}});
    assistant.tool_calls = tool_call();

    const auto body = openai()->build_chat_body(cfg, {assistant}, {}, false);
    ASSERT_EQ(assistant_message(body)["reasoning_details"][0]["text"].get<std::string>(),
              std::string("think first"));
}

TEST(openai_body_echoes_an_empty_field_when_the_provider_requires_it) {
    Config cfg;
    cfg.model_reasoning.supported = true;
    cfg.model_reasoning.echo_field = "reasoning_content";
    cfg.model_reasoning.require_echo = true;

    Message assistant;
    assistant.role = "assistant";
    assistant.content = "plain answer";

    const auto body = openai()->build_chat_body(cfg, {assistant}, {}, false);
    ASSERT_EQ(assistant_message(body)["reasoning_content"].get<std::string>(), "");
}

TEST(reasoning_effort_validation_follows_the_listing) {
    Config deepseek = config_for(only(parse(kDeepSeek)));
    ASSERT(agent::reasoning_effort_allowed(deepseek, "off"));
    ASSERT(agent::reasoning_effort_allowed(deepseek, "high"));
    ASSERT(agent::reasoning_effort_allowed(deepseek, "max"));
    ASSERT_FALSE(agent::reasoning_effort_allowed(deepseek, "medium"));
    ASSERT_FALSE(agent::reasoning_effort_allowed(deepseek, "minimal"));

    Config openrouter = config_for(only(parse(kOpenRouter)));
    ASSERT_FALSE(agent::reasoning_effort_allowed(openrouter, "off"));
    ASSERT(agent::reasoning_effort_allowed(openrouter, "xhigh"));

    Config opencode = config_for(only(parse(kOpenCode)));
    ASSERT(agent::reasoning_effort_allowed(opencode, "off"));
    ASSERT(agent::reasoning_effort_allowed(opencode, "low"));
    ASSERT_FALSE(agent::reasoning_effort_allowed(opencode, "xhigh"));
}

TEST(reasoning_badge_reports_what_the_wire_will_send) {
    Config deepseek = config_for(only(parse(kDeepSeek)));
    deepseek.reasoning_effort = "medium";
    ASSERT_EQ(agent::effective_reasoning_effort(deepseek), std::string("high"));
    deepseek.reasoning_effort = "off";
    ASSERT_EQ(agent::effective_reasoning_effort(deepseek), std::string("off"));

    Config openrouter = config_for(only(parse(kOpenRouter)));
    openrouter.reasoning_effort = "off";
    ASSERT_EQ(agent::effective_reasoning_effort(openrouter), std::string("on"));

    Config unknown;
    unknown.model = "custom/unknown";
    ASSERT_TRUE(agent::effective_reasoning_effort(unknown).empty());

    // Legacy Anthropic reaches "off" by omitting the thinking block; an
    // adaptive model that cannot disable keeps its default on.
    Config legacy;
    legacy.flavor = "anthropic";
    legacy.model_reasoning.supported = true;
    legacy.model_reasoning.legacy = true;
    legacy.model_reasoning.effort_levels = true;
    legacy.reasoning_effort = "off";
    ASSERT_EQ(agent::effective_reasoning_effort(legacy), std::string("off"));

    Config adaptive;
    adaptive.flavor = "anthropic";
    adaptive.model_reasoning.supported = true;
    adaptive.model_reasoning.adaptive = true;
    adaptive.model_reasoning.effort_levels = true;
    adaptive.model_reasoning.default_enabled = true;
    adaptive.reasoning_effort = "off";
    ASSERT_EQ(agent::effective_reasoning_effort(adaptive), std::string("on"));
}

TEST(anthropic_contract_keeps_the_adaptive_levels_in_the_ui) {
    Config cfg;
    cfg.flavor = "anthropic";
    cfg.model_reasoning.supported = true;
    cfg.model_reasoning.adaptive = true;
    cfg.model_reasoning.effort_levels = true;
    cfg.model_reasoning.disabled_ok = true;
    cfg.model_reasoning.off = ReasoningOff::ReasoningDisabled;
    cfg.model_reasoning.efforts = {"low", "medium", "high", "xhigh", "max"};
    cfg.model_reasoning.default_effort = "high";

    ASSERT(agent::reasoning_effort_allowed(cfg, "xhigh"));
    ASSERT(agent::reasoning_effort_allowed(cfg, "max"));
    ASSERT(agent::reasoning_effort_allowed(cfg, "off"));
    cfg.reasoning_effort = "max";
    ASSERT_EQ(agent::effective_reasoning_effort(cfg), std::string("max"));

    cfg.model_reasoning.mandatory = true;
    cfg.model_reasoning.off = ReasoningOff::Unsupported;
    cfg.reasoning_effort = "off";
    ASSERT_FALSE(agent::reasoning_effort_allowed(cfg, "off"));
    ASSERT_EQ(agent::effective_reasoning_effort(cfg), std::string("on"));
}
