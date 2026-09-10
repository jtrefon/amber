// PF-1.2 publish-site tests: a hermetic turn with an EventBus attached must
// produce the typed events, and hidden internal exchanges must produce none.
// Real Agent + real Context + real registry, scripted FakeLLMClient.

#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

#include "agent.h"
#include "agent/events.h"
#include "agent/tools.h"
#include "fake_llm.h"
#include "tests/test_util.h"

namespace {

std::string cwd() {
    char buf[4096];
    return getcwd(buf, sizeof buf) ? buf : ".";
}

agent::Config event_cfg() {
    agent::Config cfg;
    cfg.stream = false;
    cfg.max_tool_iterations = 100;
    cfg.system_prompt_path = "prompts/system.md";
    cfg.tools_prompt_path = "prompts/tools.md";
    return cfg;
}

void push_text(agent_test::FakeLLMClient& fake, const std::string& text) {
    agent_test::FakeReply r;
    r.content = text;
    fake.script.push_back(std::move(r));
}

void push_tool_call(agent_test::FakeLLMClient& fake, const std::string& fn,
                    const json& args, const std::string& id = "call_1") {
    agent_test::FakeReply r;
    r.tool_calls = json::array(
        {{{"id", id},
          {"type", "function"},
          {"function", {{"name", fn}, {"arguments", args.dump()}}}}});
    fake.script.push_back(std::move(r));
}

} // namespace

TEST(agent_publishes_turn_and_message_events) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = event_cfg();
    agent::ToolRegistry reg;
    reg.register_tool(agent::make_read_tool());
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    push_text(*fake, "hello there");
    push_text(*fake, "done");

    agent::EventBus bus;
    agent::Events events(bus);
    std::string seen_prompt;
    int started = 0, ended = 0, cancelled = 0, added = 0, responses = 0;
    std::vector<std::string> added_roles;

    auto s1 = events.subscribe<agent::TurnStartedEvent>(
        [&](const agent::TurnStartedEvent& e) { ++started; seen_prompt = e.prompt; });
    auto s2 = events.subscribe<agent::TurnEndedEvent>([&](const agent::TurnEndedEvent& e) {
        ++ended;
        if (e.cancelled) ++cancelled;
    });
    auto s3 = events.subscribe<agent::MessageAddedEvent>(
        [&](const agent::MessageAddedEvent& e) {
            ++added;
            if (e.message) added_roles.push_back(e.message->role);
        });
    auto s4 = events.subscribe<agent::LlmResponseEvent>(
        [&](const agent::LlmResponseEvent&) { ++responses; });

    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));
    ag.set_events(bus);
    std::string reply = ag.run("say hello");

    ASSERT_EQ(reply, std::string("hello there"));
    ASSERT_EQ(started, 1);
    ASSERT_EQ(seen_prompt, std::string("say hello"));
    ASSERT_EQ(ended, 1);
    ASSERT_EQ(cancelled, 0);
    // System prompt + user + assistant at minimum.
    ASSERT(added >= 3);
    ASSERT_EQ(added_roles.front(), std::string("system"));
    ASSERT_EQ(added_roles[1], std::string("user"));
}

TEST(agent_hidden_exchange_is_not_published) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = event_cfg();
    agent::ToolRegistry reg;
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    push_text(*fake, "answer one");
    push_text(*fake, "done");

    agent::EventBus bus;
    agent::Events events(bus);
    int responses = 0, assistant_messages = 0;
    auto s1 = events.subscribe<agent::LlmResponseEvent>(
        [&](const agent::LlmResponseEvent&) { ++responses; });
    auto s2 = events.subscribe<agent::MessageAddedEvent>(
        [&](const agent::MessageAddedEvent& e) {
            if (e.message && e.message->role == "assistant") ++assistant_messages;
        });

    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));
    ag.set_events(bus);
    ag.run("go");

    // Two model round-trips happened (reply + confirmation probe) but only the
    // visible one is published: a plugin observer must not double-count turns.
    ASSERT_EQ(raw->chat_calls, 2);
    ASSERT_EQ(responses, 1);
    ASSERT_EQ(assistant_messages, 1);
}

TEST(agent_turn_start_interceptor_rewrites_the_prompt) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = event_cfg();
    agent::ToolRegistry reg;
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeLLMClient* raw = fake.get();
    push_text(*fake, "ok");
    push_text(*fake, "done");

    agent::EventBus bus;
    agent::Events events(bus);
    auto guard = events.intercept<agent::TurnStartedEvent>(
        [](agent::TurnStartedEvent& e) { e.prompt += " [ctx injected]"; return true; });

    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));
    ag.set_events(bus);
    ag.run("original");

    bool saw_rewrite = false;
    for (const auto& m : raw->requests[0])
        if (m.role == "user" && m.content.find("[ctx injected]") != std::string::npos)
            saw_rewrite = true;
    ASSERT(saw_rewrite);
}

TEST(agent_publishes_tool_events) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = event_cfg();
    agent::ToolRegistry reg;
    reg.register_tool(agent::make_read_tool());
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    push_tool_call(*fake, "read", {{"path", "Makefile"}});
    push_text(*fake, "read it");
    push_text(*fake, "done");

    agent::EventBus bus;
    agent::Events events(bus);
    std::vector<std::string> requested, completed;
    bool saw_ok_result = false;
    auto s1 = events.subscribe<agent::ToolRequestedEvent>(
        [&](const agent::ToolRequestedEvent& e) { requested.push_back(e.name); });
    auto s2 = events.subscribe<agent::ToolCompletedEvent>(
        [&](const agent::ToolCompletedEvent& e) {
            completed.push_back(e.name);
            if (e.result && e.result->ok) saw_ok_result = true;
        });

    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));
    ag.set_events(bus);
    ag.run("read the Makefile");

    ASSERT_EQ(requested.size(), 1u);
    ASSERT_EQ(requested[0], std::string("read"));
    ASSERT_EQ(completed.size(), 1u);
    ASSERT_EQ(completed[0], std::string("read"));
    ASSERT_TRUE(saw_ok_result);
}

TEST(agent_tool_interceptor_can_veto_a_call) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = event_cfg();
    agent::ToolRegistry reg;
    reg.register_tool(agent::make_read_tool());
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    push_tool_call(*fake, "read", {{"path", "Makefile"}});
    push_text(*fake, "could not read");
    push_text(*fake, "done");

    agent::EventBus bus;
    agent::Events events(bus);
    auto guard = events.intercept<agent::ToolRequestedEvent>(
        [](agent::ToolRequestedEvent& e) {
            if (e.name == "read") {
                e.cancel = true;
                return false;
            }
            return true;
        });

    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));
    ag.set_events(bus);
    ag.run("read the Makefile");

    bool saw_denial = false;
    for (const auto& m : ag.context().get_all())
        if (m.role == "tool" && m.content.find("denied by interceptor") != std::string::npos)
            saw_denial = true;
    ASSERT(saw_denial);
}

TEST(agent_tool_interceptor_can_rewrite_arguments) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = event_cfg();
    agent::ToolRegistry reg;
    reg.register_tool(agent::make_read_tool());
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    push_tool_call(*fake, "read", {{"path", "README.md"}});
    push_text(*fake, "read something else");
    push_text(*fake, "done");

    agent::EventBus bus;
    agent::Events events(bus);
    auto guard = events.intercept<agent::ToolRequestedEvent>(
        [](agent::ToolRequestedEvent& e) {
            if (e.name == "read") e.args["path"] = "Makefile";
            return true;
        });

    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));
    ag.set_events(bus);
    ag.run("read the readme");

    bool saw_redirect = false;
    for (const auto& m : ag.context().get_all())
        if (m.role == "tool" && m.content.find("Makefile") != std::string::npos)
            saw_redirect = true;
    ASSERT(saw_redirect);
}

TEST(agent_without_bus_runs_unchanged) {
    agent::Workspace::set_root(cwd());
    agent::Config cfg = event_cfg();
    agent::ToolRegistry reg;
    reg.register_tool(agent::make_read_tool());
    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    push_text(*fake, "no bus here");
    push_text(*fake, "done");

    agent::Agent ag(cfg, reg, {}, {}, {}, {}, {}, std::move(fake));
    std::string reply = ag.run("hello");
    ASSERT_EQ(reply, std::string("no bus here"));
}
