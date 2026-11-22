// Agent journeys through the real TUI binary: a pty, a loopback provider and
// real tools. The pty regression suite pins the interface; these journeys pin
// the thing a person does with it — prompt, tool, edit, answer — through main(),
// the plugin runtime and the tool registry.

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>

#include "agent/profiles.h"
#include "agent/workspace.h"
#include "bench/scripted_endpoint.h"
#include "tests/minitest.h"
#include "tests/pty_harness.h"

namespace {

namespace fs = std::filesystem;
using agent::test::pty::require;
using agent::test::pty::Tui;
using agent::test::pty::wait_ready;

bench::EndpointReply tool_call(const std::string& id, const std::string& name,
                               const std::string& args) {
    bench::EndpointReply reply;
    agent::json call = {
        {"id", id}, {"type", "function"}, {"function", {{"name", name}, {"arguments", args}}}};
    reply.tool_calls = agent::json::array({call});
    return reply;
}

bench::EndpointReply answer(const std::string& text) {
    bench::EndpointReply reply;
    reply.content = text;
    return reply;
}

bool require_setup(bool ok, const std::string& what) {
    if (ok)
        return true;
    std::cerr << "FAIL: " << what << "\n";
    failed++;
    return false;
}

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// One journey owns an isolated root, a loopback endpoint and the pty session.
struct Journey {
    std::string name;
    std::string root;
    std::unique_ptr<bench::ScriptedEndpoint> endpoint;
    Tui tui;

    ~Journey() { tui.stop(); }

    // `prepare` runs after the isolated root exists and before the app starts,
    // for fixtures the window scans once at startup (skills, MCP servers).
    using Prepare = std::function<void(const std::string& root)>;

    bool start(const std::string& binary, const std::string& journey_name,
               std::deque<bench::EndpointReply> script, const std::string& extra_settings = "",
               const Prepare& prepare = {}, bool root_api_base = false) {
        return start(binary, journey_name,
                     std::make_unique<bench::ScriptedEndpoint>(std::move(script)), extra_settings,
                     prepare, root_api_base, false);
    }

    bool start(const std::string& binary, const std::string& journey_name,
               bench::EndpointResponder responder, const std::string& extra_settings = "",
               const Prepare& prepare = {}, bool root_api_base = false) {
        return start(binary, journey_name,
                     std::make_unique<bench::ScriptedEndpoint>(std::move(responder)),
                     extra_settings, prepare, root_api_base, false);
    }

private:
    bool start(const std::string& binary, const std::string& journey_name,
               std::unique_ptr<bench::ScriptedEndpoint> server, const std::string& extra_settings,
               const Prepare& prepare, bool root_api_base, bool already_started) {
        name = journey_name;
        root = agent::test::pty::make_isolated_root();
        if (root.empty())
            return false;
        if (prepare)
            prepare(root);
        endpoint = std::move(server);
        if (!already_started && !endpoint->start())
            return false;
        // The versioned dialects (Anthropic, Gemini) add their own /v1./v1beta
        // segment, so their api_base is the bare root.
        const std::string base = root_api_base ? endpoint->root_url() : endpoint->base_url();
        const std::string settings =
            "api_base=" + base + "\nmodel=journey-test\nstream=0\n" + extra_settings;
        if (!agent::test::pty::write_file(agent::Workspace::settings_path(), settings))
            return false;
        tui.name = name;
        return tui.start(binary, root);
    }

public:
    // A fixture that must know the endpoint URL before startup (a project pin
    // reads its profile file on the first paint) seeds it against an endpoint
    // started by the caller, then hands ownership to the journey.
    bool start_with(const std::string& binary, const std::string& journey_name,
                    std::unique_ptr<bench::ScriptedEndpoint> server,
                    const std::string& extra_settings = "", const Prepare& prepare = {},
                    bool root_api_base = false) {
        return start(binary, journey_name, std::move(server), extra_settings, prepare,
                     root_api_base, true);
    }

    std::string file(const std::string& file_name) const {
        return read_text(fs::path(root) / file_name);
    }

    bool finish(const std::string& what) {
        tui.stop();
        if (!require(tui.exit_ready_seen(), what + ": no exit-ready marker", tui))
            return false;
        return require(tui.exit_code == 0, what + ": exit code != 0", tui);
    }
};

void journey_read_edit_verify(const std::string& binary) {
    Journey journey;
    if (!require(journey.start(
                     binary, "read-edit-verify",
                     {tool_call("c1", "read", R"({"path":"a.txt"})"),
                      tool_call("c2", "write",
                                R"({"path":"a.txt","edits":[{"old":"hello","new":"hi"}]})"),
                      tool_call("c3", "read", R"({"path":"a.txt"})"), answer("JOURNEY-ONE-DONE")}),
                 "the TUI did not start", journey.tui))
        return;
    agent::test::pty::write_file(journey.root + "/a.txt", "hello\n");
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;
    journey.tui.send("Read a.txt, change hello to hi, read it back. JOURNEY-ONE\r");
    if (!require(journey.tui.wait_for("JOURNEY-ONE-DONE", 30000), "the final answer is missing",
                 journey.tui))
        return;
    require(journey.file("a.txt") == "hi\n", "the edit did not reach a.txt", journey.tui);
    require(journey.endpoint->chat_calls() == 4, "expected four chat requests", journey.tui);
    journey.finish("read-edit-verify");
}

void journey_approval_denied_falls_back(const std::string& binary) {
    Journey journey;
    journey.start(
        binary, "approval-denied",
        {tool_call("c1", "bash", R"({"command":"echo bad > /tmp/amber-pty-journey-two"})"),
         tool_call("c2", "write", R"({"path":"a.txt","edits":[{"old":"hello","new":"good"}]})"),
         answer("JOURNEY-TWO-DONE")});
    agent::test::pty::write_file(journey.root + "/a.txt", "hello\n");
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;
    journey.tui.send("Change a.txt to good. JOURNEY-TWO\r");
    if (!require(journey.tui.wait_for("Approve action?", 20000), "the approval dialog never opened",
                 journey.tui))
        return;
    journey.tui.send("4"); // Always deny
    if (!require(journey.tui.wait_for("approval: always deny", 10000),
                 "the denial was not recorded", journey.tui))
        return;
    if (!require(journey.tui.wait_for("JOURNEY-TWO-DONE", 30000),
                 "the run did not recover after the denial", journey.tui))
        return;
    require(journey.file("a.txt") == "good\n", "the fallback write did not land", journey.tui);
    journey.finish("approval-denied");
}

void journey_second_turn_carries_context(const std::string& binary) {
    int call = 0;
    Journey journey;
    if (!require(journey.start(binary, "context-carry",
                               [&call](const bench::EndpointRequest& request) {
                                   bench::EndpointReply reply;
                                   if (request.path != "/v1/chat/completions") {
                                       reply.handled = false; // the endpoint's own /v1/models
                                       return reply;
                                   }
                                   ++call;
                                   if (call == 1)
                                       return tool_call("c1", "read", R"({"path":"a.txt"})");
                                   if (call == 2)
                                       return answer("FIRST-TURN-DONE");
                                   reply.content = request.body.find("hello") != std::string::npos
                                                       ? "CONTEXT-CARRIED"
                                                       : "CONTEXT-LOST";
                                   return reply;
                               }),
                 "the TUI did not start", journey.tui))
        return;
    agent::test::pty::write_file(journey.root + "/a.txt", "hello\n");
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;
    journey.tui.send("Read a.txt. FIRST-TURN\r");
    if (!require(journey.tui.wait_for("FIRST-TURN-DONE", 30000), "the first turn did not finish",
                 journey.tui))
        return;
    journey.tui.send("What did a.txt contain? SECOND-TURN\r");
    if (!require(journey.tui.wait_for("CONTEXT-CARRIED", 30000),
                 "the second turn did not see the first turn's tool result", journey.tui))
        return;
    require(journey.endpoint->chat_calls() == 3, "expected three chat requests", journey.tui);
    journey.finish("context-carry");
}

void journey_approval_allowed(const std::string& binary) {
    const std::string marker = "/tmp/amber-tui-journey-approved";
    std::error_code ec;
    fs::remove(marker, ec);
    Journey journey;
    if (!require(journey.start(
                     binary, "approval-allowed",
                     {tool_call("c1", "bash",
                                R"({"command":"echo approved > /tmp/amber-tui-journey-approved"})"),
                      answer("JOURNEY-APPROVED-DONE")}),
                 "the TUI did not start", journey.tui))
        return;
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;
    journey.tui.send("Run the marker command. JOURNEY-APPROVE\r");
    if (!require(journey.tui.wait_for("Approve action?", 20000), "the approval dialog never opened",
                 journey.tui))
        return;
    journey.tui.send("1"); // Allow once
    if (!require(journey.tui.wait_for("approval: allowed once", 10000),
                 "the approval was not accepted", journey.tui))
        return;
    if (!require(journey.tui.wait_for("JOURNEY-APPROVED-DONE", 30000),
                 "the approved run did not finish", journey.tui))
        return;
    require(fs::exists(marker), "the approved command did not write its marker", journey.tui);
    fs::remove(marker, ec);
    journey.finish("approval-allowed");
}

void journey_cancel_slow_tool(const std::string& binary) {
    Journey journey;
    if (!require(journey.start(
                     binary, "cancel-slow-tool",
                     {tool_call("c1", "bash", R"({"command":"sleep 5; echo x > partial.txt"})"),
                      answer("AFTER-CANCEL-DONE")}),
                 "the TUI did not start", journey.tui))
        return;
    journey.tui.pump(1000);
    journey.tui.send("Run the slow command. JOURNEY-CANCEL\r");
    journey.tui.pump(1000);
    if (journey.tui.wait_for("Approve action?", 5000))
        journey.tui.send("1"); // Allow once
    if (!require(journey.tui.wait_for("bash", 15000), "the slow tool never started", journey.tui))
        return;
    journey.tui.send("/stop\r");
    if (!require(journey.tui.wait_for("stop requested", 8000), "the stop was not requested",
                 journey.tui))
        return;
    // The cancel ends the run but not the session: the prompt accepts a new
    // turn, which is the observable proof the worker returned. The tool result
    // itself may have scrolled past the failure dump, so the next turn is the
    // signal; the partial file is checked afterwards.
    journey.tui.send("Run another turn after the cancel.\r");
    if (!require(journey.tui.wait_for("AFTER-CANCEL-DONE", 15000),
                 "the TUI did not accept a turn after the cancel", journey.tui))
        return;
    require(!fs::exists(fs::path(journey.root) / "partial.txt"),
            "the cancelled tool left a partial file", journey.tui);
    journey.finish("cancel-slow-tool");
}

void journey_background_tab_keeps_its_result(const std::string& binary) {
    Journey journey;
    bench::EndpointReply slow = answer("BACKGROUND-TAB-DONE");
    slow.delay_ms = 1500;
    if (!require(journey.start(binary, "background-tab", {slow}), "the TUI did not start",
                 journey.tui))
        return;
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;
    journey.tui.send("/window new noworkdir\r");
    journey.tui.pump(800);
    journey.tui.send("/window rename worker\r");
    journey.tui.pump(500);
    journey.tui.send("Run the background task. BACKGROUND-TAB\r");
    journey.tui.pump(300);
    if (!require(journey.tui.wait_for("> Run the background task", 8000),
                 "the worker prompt was not submitted", journey.tui))
        return;
    journey.tui.send("/window set 1\r");
    journey.tui.pump(500);
    const size_t mark = journey.tui.raw.size();
    journey.tui.pump(2500); // the endpoint delay has elapsed; the worker is done
    const std::string first_tab = agent::test::pty::text_since(journey.tui, mark);
    if (!require(first_tab.find("BACKGROUND-TAB-DONE") == std::string::npos,
                 "the background result leaked into the first tab", journey.tui))
        return;
    journey.tui.send("/window set 2\r");
    if (!require(journey.tui.wait_for("BACKGROUND-TAB-DONE", 10000),
                 "the background result did not land in its own tab", journey.tui))
        return;
    journey.finish("background-tab");
}

// #621: switching tabs while an agent is still working must leave the UI
// live. No mouse input is sent anywhere in this journey: if a wheel tick is
// what restarts rendering, the prompt echo below never arrives.
void journey_tab_switch_stays_live(const std::string& binary) {
    Journey journey;
    bench::EndpointReply slow = answer("SWITCH-TAB-DONE");
    slow.delay_ms = 4000;
    if (!require(journey.start(binary, "tab-switch-live", {slow}), "the TUI did not start",
                 journey.tui))
        return;
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;

    journey.tui.send("\x0e"); // Ctrl+N: a second session tab, no drawer left open
    if (!require(journey.tui.wait_for(" 2 chat   \u00d7", 8000), "Ctrl+N did not add a tab",
                 journey.tui))
        return;
    journey.tui.send("Run the slow task. SWITCH-TAB\r");
    if (!require(journey.tui.wait_for("> Run the slow task", 8000), "the prompt was not submitted",
                 journey.tui))
        return;

    // The endpoint is still sleeping: switch away by clicking the first tab,
    // exactly the path in the report. xterm's SGR mouse mode is enabled by the
    // app; a press and a release are both required.
    if (!require(journey.tui.raw.find("\x1b[?1006") != std::string::npos,
                 "the app never enabled SGR mouse reporting", journey.tui))
        return;
    journey.tui.send("\x1b[<0;5;1M"); // SGR columns are 1-based: x=4
    journey.tui.send("\x1b[<0;5;1m");
    journey.tui.pump(500);
    const size_t list_mark = journey.tui.raw.size();
    journey.tui.send("/window list");
    journey.tui.pump(250);
    journey.tui.send("\x07");
    journey.tui.pump(500);
    if (!require(agent::test::pty::text_since(journey.tui, list_mark).find("1:amber*") !=
                     std::string::npos,
                 "clicking the tab did not switch sessions", journey.tui))
        return;
    journey.tui.send("liveness-probe");
    if (!require(journey.tui.wait_for("liveness-probe", 6000),
                 "the UI stopped accepting input after the tab switch", journey.tui))
        return;
    journey.tui.send("\x07"); // drop the probe before issuing another command

    journey.tui.send("/window set 2\r");
    if (!require(journey.tui.wait_for("SWITCH-TAB-DONE", 10000),
                 "the in-flight result was lost across the switch", journey.tui))
        return;
    journey.finish("tab-switch-live");
}

void journey_restart_resumes_session(const std::string& binary) {
    const std::string root = agent::test::pty::make_isolated_root();
    if (!require_setup(!root.empty(), "the isolated root was not created"))
        return;
    int call = 0;
    bench::ScriptedEndpoint endpoint([&call](const bench::EndpointRequest& request) {
        bench::EndpointReply reply;
        if (request.path != "/v1/chat/completions") {
            reply.handled = false;
            return reply;
        }
        ++call;
        if (call == 1)
            return tool_call("c1", "read", R"({"path":"a.txt"})");
        if (call == 2)
            return answer("RESTART-FIRST-DONE");
        reply.content = request.body.find("RESTART-FIRST-DONE") != std::string::npos
                            ? "RESTART-CONTEXT-CARRIED"
                            : "RESTART-CONTEXT-LOST";
        return reply;
    });
    if (!require_setup(endpoint.start(), "the endpoint did not start"))
        return;
    if (!require_setup(agent::test::pty::write_file(agent::Workspace::settings_path(),
                                                    "api_base=" + endpoint.base_url() +
                                                        "\nmodel=journey-test\nstream=0\n"),
                       "the settings were not written"))
        return;
    agent::test::pty::write_file(root + "/a.txt", "hello\n");
    const std::string workspace = fs::path(root).filename().string();
    {
        Tui first;
        first.name = "restart-resume (first)";
        if (!require(first.start(binary, root), "the first TUI did not start", first))
            return;
        if (!require(wait_ready(first, workspace), "the first UI never came up", first))
            return;
        first.send("Read a.txt. RESTART-FIRST\r");
        if (!require(first.wait_for("RESTART-FIRST-DONE", 30000), "the first turn did not finish",
                     first))
            return;
        first.stop();
        if (!require(first.exit_ready_seen(), "the first session did not announce its exit", first))
            return;
    }
    {
        Tui second;
        second.name = "restart-resume (second)";
        if (!require(second.start(binary, root), "the second TUI did not start", second))
            return;
        // The restored window is already painted; the startup marker and a
        // settle are enough (the prompt may be drawn as a diff of the old one).
        if (!require(second.wait_for("[" + workspace + "]\u2500", 20000),
                     "the second UI never came up", second))
            return;
        second.pump(1500);
        second.send("What did I ask you to read? RESTART-SECOND\r");
        if (!require(second.wait_for("RESTART-CONTEXT-CARRIED", 30000),
                     "the restarted session did not carry the first turn", second))
            return;
        second.stop();
        if (!require(second.exit_ready_seen(), "the second session did not announce its exit",
                     second))
            return;
    }
    endpoint.stop();
}

// The MCP fixture is a real stdio server, built next to the test tree.
std::string mcp_fixture() {
    std::error_code ec;
    const fs::path path = fs::current_path(ec) / "tests/fixtures/mcp_echo";
    return ec ? std::string() : path.string();
}

void journey_subagent_reports_back(const std::string& binary) {
    int child_requests = 0;
    Journey journey;
    if (!require(
            journey.start(
                binary, "subagent-worker",
                [&child_requests](const bench::EndpointRequest& request) {
                    bench::EndpointReply reply;
                    if (request.path != "/v1/chat/completions") {
                        reply.handled = false;
                        return reply;
                    }
                    std::string last_user;
                    for (const agent::Message& message : request.messages)
                        if (message.role == "user")
                            last_user = message.content;
                    if (last_user.find("JOURNEY-SUBAGENT-GOAL") != std::string::npos) {
                        ++child_requests;
                        reply.content = "SUBAGENT-REPORT-DONE: the workspace is clean";
                        return reply;
                    }
                    if (request.body.find("SUBAGENT-REPORT-DONE") != std::string::npos) {
                        reply.content = "PARENT-SAW-SUBAGENT";
                        return reply;
                    }
                    if (request.body.find("call_delegate") != std::string::npos) {
                        reply.content = "SUBAGENT-STARTED";
                        return reply;
                    }
                    return tool_call(
                        "call_delegate", "delegate",
                        R"({"goal":"JOURNEY-SUBAGENT-GOAL: inspect the workspace","context":"the workspace root","expect":"one short report","label":"journey-worker"})");
                }),
            "the TUI did not start", journey.tui))
        return;
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;
    journey.tui.send("Delegate the workspace report. JOURNEY-SUBAGENT\r");
    if (!require(journey.tui.wait_for("Approve action?", 20000),
                 "the delegation approval never opened", journey.tui))
        return;
    journey.tui.send("1"); // Allow the worker
    if (!require(journey.tui.wait_for("PARENT-SAW-SUBAGENT", 30000),
                 "the parent never saw the worker's report", journey.tui))
        return;
    require(child_requests == 1, "the worker made more than one request", journey.tui);
    journey.tui.send("/tasks list\r");
    if (!require(journey.tui.wait_for("journey-worker", 8000),
                 "the worker was not listed in /tasks", journey.tui))
        return;
    journey.finish("subagent-worker");
}

void journey_skill_activation(const std::string& binary) {
    int calls = 0;
    Journey journey;
    if (!require(journey.start(
                     binary, "skill-activation",
                     [&calls](const bench::EndpointRequest& request) {
                         bench::EndpointReply reply;
                         if (request.path != "/v1/chat/completions") {
                             reply.handled = false;
                             return reply;
                         }
                         ++calls;
                         if (calls == 1)
                             return tool_call("c1", "read_skill", R"({"name":"journey-skill"})");
                         reply.content =
                             request.body.find("JOURNEY-SKILL-BODY-MARKER") != std::string::npos
                                 ? "SKILL-ACTIVATED"
                                 : "SKILL-BODY-MISSING";
                         return reply;
                     },
                     "",
                     [](const std::string& root) {
                         const std::string dir = root + "/.amber/skills/journey-skill";
                         std::error_code ec;
                         fs::create_directories(dir, ec);
                         require_setup(
                             agent::test::pty::write_file(
                                 dir + "/SKILL.md",
                                 "---\nname: journey-skill\ndescription: Journey skill body "
                                 "check.\n---\n\nWhen active, the model has this marker: "
                                 "JOURNEY-SKILL-BODY-MARKER\n"),
                             "the journey skill was not written");
                     }),
                 "the TUI did not start", journey.tui))
        return;
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;
    journey.tui.send("Activate the journey skill. JOURNEY-SKILL\r");
    if (!require(journey.tui.wait_for("SKILL-ACTIVATED", 30000),
                 "the activated skill body never reached the model", journey.tui))
        return;
    require(calls == 2, "the activation did not take exactly two requests", journey.tui);
    journey.finish("skill-activation");
}

void journey_mcp_tool_call(const std::string& binary) {
    const std::string fixture = mcp_fixture();
    if (!require_setup(fs::exists(fixture), "the mcp_echo fixture is missing; run make test"))
        return;
    Journey journey;
    if (!require(journey.start(
                     binary, "mcp-tool",
                     [](const bench::EndpointRequest& request) {
                         bench::EndpointReply reply;
                         if (request.path != "/v1/chat/completions") {
                             reply.handled = false;
                             return reply;
                         }
                         if (request.body.find("echo:JOURNEY-MCP") != std::string::npos) {
                             reply.content = "MCP-TOOL-DONE";
                             return reply;
                         }
                         return tool_call("c1", "mcp_echo_echo_tool", R"({"text":"JOURNEY-MCP"})");
                     },
                     "",
                     [&fixture](const std::string& root) {
                         const std::string dir = root + "/.amber/mcp";
                         std::error_code ec;
                         fs::create_directories(dir, ec);
                         require_setup(agent::test::pty::write_file(
                                           dir + "/echo.conf",
                                           "type=stdio\ncommand=" + fixture + "\ncwd=" + root +
                                               "\nenabled=1\nauto_connect=1\ntrusted=1\n"),
                                       "the MCP server config was not written");
                     }),
                 "the TUI did not start", journey.tui))
        return;
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;
    // Auto-connect is asynchronous: its tools exist once the server reports
    // them, so wait for the inventory rather than sleeping.
    journey.tui.send("/mcp list\r");
    if (!require(journey.tui.wait_for("1 tools", 20000), "the MCP server never connected",
                 journey.tui))
        return;
    journey.tui.send("Call the echo tool. JOURNEY-MCP\r");
    if (!require(journey.tui.wait_for("MCP-TOOL-DONE", 30000),
                 "the MCP tool result never reached the model", journey.tui))
        return;
    journey.tui.send("/mcp show echo\r");
    if (!require(journey.tui.wait_for("echo_tool", 8000), "/mcp show did not list the server tool",
                 journey.tui))
        return;
    journey.finish("mcp-tool");
}

void journey_compression_keeps_ui_responsive(const std::string& binary) {
    std::atomic<bool> classifying{false};
    std::atomic<int> classify_calls{0};
    int reads = 0;
    Journey journey;
    if (!require(
            journey.start(
                binary, "compression-responsive",
                [&classifying, &classify_calls, &reads](const bench::EndpointRequest& request) {
                    bench::EndpointReply reply;
                    if (request.path != "/v1/chat/completions") {
                        reply.handled = false;
                        return reply;
                    }
                    std::string last_user;
                    for (const agent::Message& message : request.messages)
                        if (message.role == "user")
                            last_user = message.content;
                    // The extract request replays the classify one as its
                    // cached prefix, so it has to be matched first.
                    if (request.body.find("Review the classification above") != std::string::npos) {
                        reply.content = R"({"memories":[],"skills":[]})";
                        return reply;
                    }
                    if (request.body.find("Analyze the conversation above") != std::string::npos) {
                        ++classify_calls;
                        classifying = true;
                        reply.delay_ms = 2000;
                        reply.content =
                            R"({"summary":"the agent read a.txt four times; it contains hello world","classification":[{"turns":"0-6","tag":"context","summary":"read a.txt"}]})";
                        return reply;
                    }
                    if (last_user.find("AFTER-COMPRESSION-PROMPT") != std::string::npos) {
                        reply.content = "AFTER-COMPRESSION-DONE";
                        return reply;
                    }
                    if (++reads <= 4) {
                        reply =
                            tool_call("c" + std::to_string(reads), "read", R"({"path":"a.txt"})");
                        return reply;
                    }
                    reply.content = "READS-DONE: hello world";
                    return reply;
                },
                // The window is wide enough that the post-compression estimate
                // does not trip the automatic gate: this journey is about the
                // manual /compress worker, and a second pass would hide it.
                "context_size=32000\n"),
            "the TUI did not start", journey.tui))
        return;
    agent::test::pty::write_file(journey.root + "/a.txt", "hello world\n");
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;
    journey.tui.send("Read a.txt four times. JOURNEY-COMPRESS\r");
    if (!require(journey.tui.wait_for("READS-DONE", 30000), "the read turn never finished",
                 journey.tui))
        return;
    journey.tui.send("/compress\r");
    if (!require(journey.tui.wait_for("compressing...", 8000),
                 "the manual compression never started", journey.tui))
        return;
    bool seen = false;
    for (int waited = 0; waited < 20000 && !seen; waited += 50) {
        seen = classifying.load();
        if (!seen)
            journey.tui.pump(50);
    }
    if (!require(seen, "the compression pass never reached the classifier", journey.tui))
        return;
    journey.tui.send("Ask after the compression. AFTER-COMPRESSION-PROMPT\r");
    if (!require(journey.tui.wait_for("queued (1 in queue)", 8000),
                 "the UI did not accept input while the compression ran", journey.tui))
        return;
    if (!require(journey.tui.wait_for("AFTER-COMPRESSION-DONE", 30000),
                 "the queued prompt did not run after the compression", journey.tui))
        return;
    require(classify_calls == 1, "the compression ran more than once", journey.tui);
    require(journey.endpoint->chat_calls() == 8,
            "expected four reads, one answer, classify, extract and a queued answer", journey.tui);
    journey.finish("compression-responsive");
}

void journey_plugin_tool(const std::string& binary) {
    Journey journey;
    if (!require(journey.start(binary, "plugin-tool",
                               {tool_call("c1", "plugin_fake_echo", R"({"text":"TUI-PLUGIN"})"),
                                answer("TUI-PLUGIN-DONE")},
                               "",
                               [](const std::string& root) {
                                   const std::string dir = root + "/.amber/plugins/fake";
                                   std::error_code ec;
                                   fs::create_directories(dir, ec);
                                   fs::copy_file("tests/plugins/fake_manifest.json",
                                                 dir + "/manifest.json",
                                                 fs::copy_options::overwrite_existing, ec);
                                   fs::copy_file("tests/plugins/fake_plugin.py",
                                                 dir + "/fake_plugin.py",
                                                 fs::copy_options::overwrite_existing, ec);
                                   chmod((dir + "/fake_plugin.py").c_str(), 0755);
                               }),
                 "the TUI did not start", journey.tui))
        return;
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;
    journey.tui.send("/set plugin on fake\r");
    if (!require(journey.tui.wait_for("plugin fake on", 8000),
                 "the external plugin never activated", journey.tui))
        return;
    journey.tui.send("Call the plugin tool. TUI-PLUGIN\r");
    if (!require(journey.tui.wait_for("Approve action?", 15000),
                 "the plugin tool's approval never opened", journey.tui))
        return;
    journey.tui.send("1"); // Allow once
    if (!require(journey.tui.wait_for("TUI-PLUGIN-DONE", 30000),
                 "the plugin tool did not complete the turn", journey.tui))
        return;
    require(journey.tui.text().find("echo:TUI-PLUGIN") != std::string::npos,
            "the plugin's output did not reach the transcript", journey.tui);
    journey.finish("plugin-tool");
}

void journey_provider_dialect(const std::string& binary) {
    Journey journey;
    if (!require(
            journey.start(
                binary, "provider-anthropic",
                [](const bench::EndpointRequest& request) {
                    bench::EndpointReply reply;
                    if (request.path != "/v1/messages") {
                        reply.handled = false; // the endpoint's own /v1/models
                        return reply;
                    }
                    reply.content_type = "application/json";
                    reply.body =
                        R"({"id":"msg_tui_1","type":"message","role":"assistant","model":"journey-test","content":[{"type":"text","text":"TUI-ANTHROPIC-DONE"}],"stop_reason":"end_turn","stop_sequence":null,"usage":{"input_tokens":10,"output_tokens":2}})";
                    return reply;
                },
                "flavor=anthropic\n", {}, true),
            "the TUI did not start", journey.tui))
        return;
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;
    journey.tui.send("Reply done. TUI-ANTHROPIC\r");
    if (!require(journey.tui.wait_for("TUI-ANTHROPIC-DONE", 30000),
                 "the Anthropic dialect answer never arrived", journey.tui))
        return;
    bool saw_messages = false;
    for (const std::string& path : journey.endpoint->paths())
        saw_messages = saw_messages || path == "/v1/messages";
    require(saw_messages, "the client did not use the Messages API path", journey.tui);
    journey.finish("provider-anthropic");
}

// A listing that advertises a capability must reach the wire, and switching
// to a model that advertises nothing must clear it. Unit tests cover the
// parsing; this journey drives the real binary through the catalogue fetch,
// the capability application and all three request shapes.
void journey_capability_ingestion(const std::string& binary) {
    const std::string catalogue = R"({"models":[
      {"name":"models/cap-thinking","inputTokenLimit":131072,"outputTokenLimit":8192,
       "thinking":{"supported":true,"default_enabled":true,"mandatory":false,
                   "supported_levels":["low","high"],"default_level":"high"},
       "verbosity":{"supported_levels":["low","medium","high"],"default_level":"medium"}},
      {"name":"models/cap-plain","inputTokenLimit":32768,"outputTokenLimit":4096}
    ]})";
    Journey journey;
    if (!require(
            journey.start(
                binary, "capability-ingestion",
                [&catalogue](const bench::EndpointRequest& request) {
                    bench::EndpointReply reply;
                    if (request.path == "/v1beta/models") {
                        reply.body = catalogue;
                        return reply;
                    }
                    if (request.path.find(":generateContent") == std::string::npos) {
                        reply.handled = false;
                        return reply;
                    }
                    if (request.sequence == 0)
                        reply.body =
                            R"({"candidates":[{"content":{"role":"model","parts":[)"
                            R"({"text":"CAP-THINKING-ON-DONE"}]},"finishReason":"STOP","index":0}],)"
                            R"("usageMetadata":{"promptTokenCount":10,"candidatesTokenCount":2}})";
                    else if (request.sequence == 1)
                        reply.body =
                            R"({"candidates":[{"content":{"role":"model","parts":[)"
                            R"({"text":"CAP-THINKING-OFF-DONE"}]},"finishReason":"STOP","index":0}],)"
                            R"("usageMetadata":{"promptTokenCount":10,"candidatesTokenCount":2}})";
                    else
                        reply.body =
                            R"({"candidates":[{"content":{"role":"model","parts":[)"
                            R"({"text":"CAP-PLAIN-DONE"}]},"finishReason":"STOP","index":0}],)"
                            R"("usageMetadata":{"promptTokenCount":10,"candidatesTokenCount":2}})";
                    return reply;
                },
                "flavor=gemini\nmodel=cap-thinking\nreasoning_effort=high\napi_key=sk-test\n", {},
                true),
            "the TUI did not start", journey.tui))
        return;

    // Startup reads the disk cache only; the catalogue arrives on a worker.
    bool listed = false;
    for (int waited = 0; waited < 30000 && !listed; waited += 200) {
        journey.tui.pump(200);
        for (const auto& request : journey.endpoint->requests())
            listed = listed || request.path == "/v1beta/models";
    }
    if (!require(listed, "the Gemini catalogue was never fetched", journey.tui))
        return;
    if (!require(journey.tui.wait_for("n_ctx=131072", 20000),
                 "the fetched catalogue was never applied to the session", journey.tui))
        return;

    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;
    journey.tui.send("Reply with the word done. CAP-ON\r");
    if (!require(journey.tui.wait_for("CAP-THINKING-ON-DONE", 30000),
                 "the thinking-on answer never arrived", journey.tui))
        return;

    journey.tui.send("/set reasoning off\r");
    if (!require(journey.tui.wait_for("reasoning effort: off", 10000),
                 "the reasoning effort switch did not land", journey.tui))
        return;
    journey.tui.send("Reply with the word done. CAP-OFF\r");
    if (!require(journey.tui.wait_for("CAP-THINKING-OFF-DONE", 30000),
                 "the thinking-off answer never arrived", journey.tui))
        return;

    journey.tui.send("/model cap-plain\r");
    if (!require(journey.tui.wait_for("model set to cap-plain", 10000),
                 "the model switch did not land", journey.tui))
        return;
    journey.tui.send("Reply with the word done. CAP-PLAIN\r");
    if (!require(journey.tui.wait_for("CAP-PLAIN-DONE", 30000),
                 "the quiet-model answer never arrived", journey.tui))
        return;

    std::vector<agent::json> chat;
    for (const auto& request : journey.endpoint->requests())
        if (request.path.find(":generateContent") != std::string::npos)
            chat.push_back(request.json);
    if (!require(chat.size() == 3u, "expected three Gemini chat requests", journey.tui))
        return;
    const auto config_of = [](const agent::json& body) -> agent::json {
        const auto gen = body.find("generationConfig");
        if (gen == body.end() || !gen->is_object())
            return {};
        const auto cfg = gen->find("thinkingConfig");
        return cfg == gen->end() ? agent::json{} : *cfg;
    };
    const agent::json on = config_of(chat[0]);
    const agent::json off = config_of(chat[1]);
    const agent::json quiet = config_of(chat[2]);
    if (!require(on.value("includeThoughts", false),
                 "the advertised thinking contract did not reach the wire", journey.tui))
        return;
    if (!require(!on.contains("thinkingBudget"), "the journey invented a numeric thinking budget",
                 journey.tui))
        return;
    if (!require(off.value("thinkingBudget", -1) == 0,
                 "thinking off did not use Gemini's documented zero budget", journey.tui))
        return;
    if (!require(!off.contains("includeThoughts"), "thinking off still asked for thoughts",
                 journey.tui))
        return;
    if (!require(quiet.empty(), "the quiet model inherited the previous thinking contract",
                 journey.tui))
        return;
    if (!require(!chat[0].contains("verbosity"),
                 "a listing-derived verbosity was sent without a wire contract", journey.tui))
        return;
    journey.finish("capability-ingestion");
}

// Reasoning tokens are a breakdown of `down`, never a second charge. The unit
// suite pins the parser; these journeys pin the readout and the session round
// trip through the real binary, including the provider that reports no split.
void journey_usage_reasoning_tokens(const std::string& binary) {
    const auto responder = [](bool report_reasoning) {
        return [report_reasoning](const bench::EndpointRequest& request) {
            bench::EndpointReply reply;
            if (request.path != "/v1/chat/completions") {
                reply.handled = false;
                return reply;
            }
            const std::string split =
                report_reasoning ? R"(,"completion_tokens_details":{"reasoning_tokens":8})" : "";
            reply.body =
                std::string(R"({"id":"chatcmpl-usage","object":"chat.completion","created":1,)") +
                R"("model":"journey-test","choices":[{"index":0,"message":{"role":"assistant",)"
                R"("content":"USAGE-REASONING-DONE"},"finish_reason":"stop"}],)"
                R"("usage":{"prompt_tokens":10,"completion_tokens":20,"total_tokens":30)" +
                split + R"(}})";
            return reply;
        };
    };

    // A provider that splits out reasoning tokens: the readout names them and
    // the session meta survives the quit path.
    Journey reported;
    if (!require(reported.start(binary, "usage-reasoning", responder(true)),
                 "the TUI did not start", reported.tui))
        return;
    if (!require(wait_ready(reported.tui, fs::path(reported.root).filename().string()),
                 "the UI never came up", reported.tui))
        return;
    reported.tui.send("Reasoning probe. USAGE-REASONING\r");
    if (!require(reported.tui.wait_for("USAGE-REASONING-DONE", 30000), "the reply did not arrive",
                 reported.tui))
        return;
    reported.tui.send("/usage");
    reported.tui.pump(250);
    reported.tui.send("\x07");
    if (!require(reported.tui.wait_for("thinking 8", 10000),
                 "/usage did not report the reasoning breakdown", reported.tui))
        return;
    if (!require(reported.tui.text().find("down 20") != std::string::npos,
                 "reasoning was added to `down` instead of broken out of it", reported.tui))
        return;
    if (!require(reported.finish("usage-reasoning"), "the TUI did not exit cleanly", reported.tui))
        return;

    bool persisted = false;
    const fs::path state = fs::path(reported.root) / "state";
    std::error_code ec;
    for (fs::recursive_directory_iterator it(state, ec), end; it != end; it.increment(ec)) {
        if (!it->is_regular_file() || it->path().filename() == "workspace.json" ||
            it->path().extension() != ".json")
            continue;
        const agent::json session = agent::json::parse(read_text(it->path()), nullptr, false);
        if (session.is_discarded() || !session.contains("meta"))
            continue;
        const auto& meta = session["meta"];
        if (meta.value("session_usage_main_reasoning_tokens", 0) == 8 &&
            meta.value("session_usage_main_reasoning_measured", false))
            persisted = true;
    }
    require(persisted, "the reasoning total did not survive in the session meta", reported.tui);

    // A provider that reports no split: no breakdown and never a fabricated 0.
    Journey quiet;
    if (!require(quiet.start(binary, "usage-no-reasoning", responder(false)),
                 "the TUI did not start", quiet.tui))
        return;
    if (!require(wait_ready(quiet.tui, fs::path(quiet.root).filename().string()),
                 "the UI never came up", quiet.tui))
        return;
    quiet.tui.send("Reasoning probe. USAGE-REASONING\r");
    if (!require(quiet.tui.wait_for("USAGE-REASONING-DONE", 30000), "the reply did not arrive",
                 quiet.tui))
        return;
    quiet.tui.send("/usage");
    quiet.tui.pump(250);
    quiet.tui.send("\x07");
    if (!require(quiet.tui.wait_for("main: up 10 down 20", 10000),
                 "/usage did not report the measured totals", quiet.tui))
        return;
    require(quiet.tui.text().find("thinking ") == std::string::npos,
            "/usage fabricated a reasoning breakdown the provider never reported", quiet.tui);
    quiet.finish("usage-no-reasoning");
}

// Profiles are connection state, not a readout: a switch must change which
// endpoint receives the next turn, mask its key, wait for a busy turn and be
// pinnable by the project. Both profiles use one loopback endpoint with two
// path prefixes so the request itself says which connection was adopted.
void journey_profile_switch(const std::string& binary) {
    const std::string default_key = "sk-default-0000000000";
    const std::string work_key = "sk-work-1234567890abcd";
    const auto seed_profile = [](const char* name, const char* provider, const char* model,
                                 const std::string& base, const std::string& key) {
        agent::Profile p;
        p.name = name;
        p.provider_name = provider;
        p.flavor = "openai";
        p.api_base = base;
        p.api_key = key;
        p.model = model;
        std::string error;
        return agent::save_profile(p, &error);
    };
    auto chat_seq = std::make_shared<int>(0);
    const auto responder = [chat_seq](const bench::EndpointRequest& request) {
        bench::EndpointReply reply;
        if (request.path.find("/models") != std::string::npos) {
            reply.body = R"({"data":[{"id":"journey-default","context_length":8192},)"
                         R"({"id":"journey-work","context_length":8192}]})";
            return reply;
        }
        const bool work = request.path.find("/work/") != std::string::npos;
        reply.body =
            std::string(R"({"id":"chatcmpl-profile","object":"chat.completion","created":1,)") +
            R"("model":"journey-test","choices":[{"index":0,"message":{"role":"assistant",)"
            R"("content":")" +
            (work ? "WORK-PROFILE-DONE" : "DEFAULT-PROFILE-DONE") +
            R"("},"finish_reason":"stop"}],)"
            R"("usage":{"prompt_tokens":1,"completion_tokens":1,"total_tokens":2}})";
        // The turn that receives a queued switch must still be running when the
        // command arrives; the echo alone is not enough to prove that.
        if ((*chat_seq)++ == 2)
            reply.delay_ms = 1500;
        return reply;
    };

    Journey journey;
    {
        auto server = std::make_unique<bench::ScriptedEndpoint>(responder);
        if (!require(server->start(), "the loopback endpoint did not start", journey.tui))
            return;
        const std::string live = server->base_url();
        const std::string work_base = server->root_url() + "/work/v1";
        if (!require(journey.start_with(binary, "profile-switch", std::move(server),
                                        "model=journey-default\n",
                                        [&](const std::string&) {
                                            seed_profile("default", "custom", "journey-default",
                                                         live, default_key);
                                            seed_profile("work", "work-gateway", "journey-work",
                                                         work_base, work_key);
                                        }),
                     "the TUI did not start", journey.tui))
            return;
    }
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;

    journey.tui.send("/profile list\r");
    if (!require(journey.tui.wait_for("work-gateway / journey-work", 10000),
                 "/profile list did not show the work connection", journey.tui))
        return;
    if (!require(journey.tui.wait_for("sk-...abcd", 10000), "/profile list did not mask the key",
                 journey.tui))
        return;

    journey.tui.send("Reply once. DEFAULT-TURN\r");
    if (!require(journey.tui.wait_for("DEFAULT-PROFILE-DONE", 30000),
                 "the first turn did not use the settings connection", journey.tui))
        return;
    journey.tui.send("/profile switch work\r");
    if (!require(journey.tui.wait_for("profile: work (work-gateway, journey-work)", 10000),
                 "the profile switch did not land", journey.tui))
        return;
    journey.tui.send("Reply once. WORK-TURN\r");
    if (!require(journey.tui.wait_for("WORK-PROFILE-DONE", 30000),
                 "the switch did not change the request's endpoint", journey.tui))
        return;

    // A switch asked while the next turn is running must queue, not replace the
    // client that turn is using.
    journey.tui.send("Reply slowly. BUSY-TURN\r");
    if (!require(journey.tui.wait_for("> Reply slowly. BUSY-TURN", 10000),
                 "the busy turn was not submitted", journey.tui))
        return;
    journey.tui.send("/profile switch default\r");
    if (!require(journey.tui.wait_for("profile: switch to default queued", 10000),
                 "the busy switch was not queued", journey.tui))
        return;
    if (!require(journey.tui.wait_for("profile: default (custom, journey-default)", 30000),
                 "the queued switch never landed", journey.tui))
        return;

    journey.tui.send("/profile switch ghost\r");
    if (!require(journey.tui.wait_for("there is no profile named ghost", 10000),
                 "a missing profile did not warn", journey.tui))
        return;
    if (!require(journey.tui.wait_for("profile: default (custom, journey-default)", 10000),
                 "the missing profile changed the connection", journey.tui))
        return;
    if (!require(journey.finish("profile-switch"), "the TUI did not exit cleanly", journey.tui))
        return;

    // Credentials belong to the profile files, never to the workspace or the
    // session meta written under the state dir.
    std::error_code ec;
    for (fs::recursive_directory_iterator it(journey.root, ec), end; it != end; it.increment(ec)) {
        // The XDG config tree legitimately holds keys (profiles + global
        // config); the workspace and the session meta under /state must not.
        if (!it->is_regular_file() || it->path().string().find("/xdg/") != std::string::npos)
            continue;
        const std::string content = read_text(it->path());
        if (!require(content.find(default_key) == std::string::npos &&
                         content.find(work_key) == std::string::npos,
                     "a profile key leaked into " + it->path().string(), journey.tui))
            return;
    }

    // The project pin is a one-line file and must apply before the first turn,
    // so its profile is seeded against the live endpoint before startup.
    Journey pinned;
    {
        auto server = std::make_unique<bench::ScriptedEndpoint>(responder);
        if (!require(server->start(), "the pinned endpoint did not start", pinned.tui))
            return;
        const std::string work_base = server->root_url() + "/work/v1";
        if (!require(pinned.start_with(binary, "profile-project-pin", std::move(server),
                                       "model=journey-default\n",
                                       [&](const std::string& root) {
                                           seed_profile("work", "work-gateway", "journey-work",
                                                        work_base, work_key);
                                           fs::create_directories(fs::path(root) / ".amber", ec);
                                           agent::test::pty::write_file(root + "/.amber/profile",
                                                                        "work\n");
                                       }),
                     "the pinned TUI did not start", pinned.tui))
            return;
    }
    if (!require(wait_ready(pinned.tui, fs::path(pinned.root).filename().string()),
                 "the pinned UI never came up", pinned.tui))
        return;
    pinned.tui.send("Reply once. PIN-TURN\r");
    require(pinned.tui.wait_for("WORK-PROFILE-DONE", 30000),
            "the project pin did not apply before the first turn", pinned.tui);
    pinned.finish("profile-project-pin");
}

// Re-run the journeys in fresh processes. Each journey already owns an
// isolated root, but a fresh process also resets the plugin runtime, the model
// cache and every static the TUI touches.
int repeat_suite(const std::string& self, const std::string& binary, int repeat) {
    for (int iteration = 1; iteration <= repeat; ++iteration) {
        const pid_t child = fork();
        if (child < 0) {
            std::cerr << "FAIL: could not fork for --repeat\n";
            return 1;
        }
        if (child == 0) {
            execlp(self.c_str(), self.c_str(), binary.c_str(), static_cast<char*>(nullptr));
            std::fprintf(stderr, "repeat child: exec(%s): %s\n", self.c_str(),
                         std::strerror(errno));
            _exit(127);
        }
        int status = 0;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
        const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        std::cout << "--- repeat " << iteration << "/" << repeat << ": "
                  << (ok ? "passed" : "FAILED") << "\n";
        if (!ok)
            return 1;
    }
    return 0;
}

void report_process_failures() {
    for (const std::string& session : agent::test::pty::killed_sessions()) {
        std::cerr << "FAIL: session '" << session
                  << "' was force-killed instead of exiting through the quit path\n";
        failed++;
    }
    for (const std::string& session : agent::test::pty::sessions_without_exit_ready()) {
        std::cerr << "FAIL: session '" << session << "' exited without its exit-ready file\n";
        failed++;
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string binary_arg = "./amber";
    std::string self_arg = argc > 0 ? argv[0] : "tui_agent_e2e_test";
    int repeat = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--repeat" && i + 1 < argc) {
            repeat = std::max(1, std::atoi(argv[++i]));
            continue;
        }
        binary_arg = arg;
    }
    if (repeat > 1) {
        std::error_code ignored;
        const fs::path self = fs::absolute(self_arg, ignored);
        return repeat_suite(ignored ? self_arg : self.string(), binary_arg, repeat);
    }
    const std::string binary = fs::absolute(binary_arg).lexically_normal().string();
    const char* only = std::getenv("AMBER_TUI_JOURNEY");
    if (!only || std::string(only) == "context")
        journey_second_turn_carries_context(binary);
    if (!only || std::string(only) == "approval")
        journey_approval_denied_falls_back(binary);
    if (!only || std::string(only) == "approval-allowed")
        journey_approval_allowed(binary);
    if (!only || std::string(only) == "cancel")
        journey_cancel_slow_tool(binary);
    if (!only || std::string(only) == "tab")
        journey_background_tab_keeps_its_result(binary);
    if (!only || std::string(only) == "switch")
        journey_tab_switch_stays_live(binary);
    if (!only || std::string(only) == "restart")
        journey_restart_resumes_session(binary);
    if (!only || std::string(only) == "subagent")
        journey_subagent_reports_back(binary);
    if (!only || std::string(only) == "skill")
        journey_skill_activation(binary);
    if (!only || std::string(only) == "mcp")
        journey_mcp_tool_call(binary);
    if (!only || std::string(only) == "plugin")
        journey_plugin_tool(binary);
    if (!only || std::string(only) == "provider")
        journey_provider_dialect(binary);
    if (!only || std::string(only) == "capability")
        journey_capability_ingestion(binary);
    if (!only || std::string(only) == "usage")
        journey_usage_reasoning_tokens(binary);
    if (!only || std::string(only) == "profile")
        journey_profile_switch(binary);
    if (!only || std::string(only) == "compression")
        journey_compression_keeps_ui_responsive(binary);
    if (!only)
        journey_read_edit_verify(binary);
    report_process_failures();
    if (failed)
        std::cout << "FAILED (" << failed << " failures)\n";
    else
        std::cout << "ALL PASSED\n";
    return failed ? 1 : 0;
}
