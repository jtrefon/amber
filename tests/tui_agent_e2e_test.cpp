// Agent journeys through the real TUI binary: a pty, a loopback provider and
// real tools. The pty regression suite pins the interface; these journeys pin
// the thing a person does with it — prompt, tool, edit, answer — through main(),
// the plugin runtime and the tool registry.

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

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

    bool start(const std::string& binary, const std::string& journey_name,
               std::deque<bench::EndpointReply> script) {
        return start(binary, journey_name,
                     std::make_unique<bench::ScriptedEndpoint>(std::move(script)));
    }

    bool start(const std::string& binary, const std::string& journey_name,
               bench::EndpointResponder responder) {
        return start(binary, journey_name,
                     std::make_unique<bench::ScriptedEndpoint>(std::move(responder)));
    }

private:
    bool start(const std::string& binary, const std::string& journey_name,
               std::unique_ptr<bench::ScriptedEndpoint> server) {
        name = journey_name;
        root = agent::test::pty::make_isolated_root();
        if (root.empty())
            return false;
        endpoint = std::move(server);
        if (!endpoint->start())
            return false;
        const std::string settings =
            "api_base=" + endpoint->base_url() + "\nmodel=journey-test\nstream=0\n";
        if (!agent::test::pty::write_file(agent::Workspace::settings_path(), settings))
            return false;
        tui.name = name;
        return tui.start(binary, root);
    }

public:
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
    if (!only)
        journey_read_edit_verify(binary);
    report_process_failures();
    if (failed)
        std::cout << "FAILED (" << failed << " failures)\n";
    else
        std::cout << "ALL PASSED\n";
    return failed ? 1 : 0;
}
