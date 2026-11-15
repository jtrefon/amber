#include "bench/binary_driver.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "bench/mock_server.h"
#include "bench/report.h"
#include "bench/runner.h"

namespace fs = std::filesystem;

namespace bench {

namespace {

void write_setup_files(const fs::path& workspace, const agent::json& setup) {
    if (!setup.contains("files") || !setup["files"].is_object())
        return;
    for (auto it = setup["files"].begin(); it != setup["files"].end(); ++it) {
        const fs::path path = workspace / it.key();
        fs::create_directories(path.parent_path());
        std::ofstream out(path);
        out << (it.value().is_string() ? it.value().get<std::string>() : it.value().dump());
    }
}

std::deque<MockReply> replies_for(const Scenario& scenario) {
    std::deque<MockReply> replies;
    for (const auto& entry : scenario.fake_replies) {
        if (!entry.is_object())
            continue;
        MockReply reply;
        reply.content = entry.value("content", "");
        if (entry.contains("tool_calls"))
            reply.tool_calls = entry["tool_calls"];
        reply.prompt_tokens = entry.value("prompt_tokens", 0L);
        reply.completion_tokens = entry.value("completion_tokens", 0L);
        replies.push_back(std::move(reply));
    }
    return replies;
}

std::string temp_workspace(std::string& err) {
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec);
    if (ec) {
        err = "cannot find the temporary directory: " + ec.message();
        return {};
    }
    std::string tmpl = (base / "amber-e2e-XXXXXX").string();
    std::vector<char> buffer(tmpl.begin(), tmpl.end());
    buffer.push_back('\0');
    if (!mkdtemp(buffer.data())) {
        err = "cannot create a temporary workspace";
        return {};
    }
    return buffer.data();
}

std::vector<std::string> expected_tools(const Scenario& scenario, std::string& err) {
    RunOptions opts;
    RunMeta meta;
    meta.mode = "hermetic";
    meta.model = "fake";
    const ScenarioReport report = run_one_scenario(scenario, opts, meta, err);
    std::vector<std::string> tools;
    for (const auto& call : report.tool_calls) {
        const std::size_t suffix = call.first.find(" [");
        tools.emplace_back(call.first.substr(0, suffix));
    }
    return tools;
}

int run_cli(const std::string& cli_path, const std::vector<std::string>& args, const fs::path& ws,
            std::string& err) {
    const pid_t pid = fork();
    if (pid < 0) {
        err = "fork failed";
        return -1;
    }
    if (pid == 0) {
        setenv("AMBER_WORKSPACE", ws.string().c_str(), 1);
        const std::string xdg = (ws / "xdg").string();
        setenv("XDG_CONFIG_HOME", (xdg + "/config").c_str(), 1);
        setenv("XDG_STATE_HOME", (xdg + "/state").c_str(), 1);
        setenv("XDG_CACHE_HOME", (xdg + "/cache").c_str(), 1);
        setenv("HOME", ws.string().c_str(), 1);
        const int out = open((ws / "out.txt").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        const int err_file = open((ws / "err.txt").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (out >= 0)
            dup2(out, STDOUT_FILENO);
        if (err_file >= 0)
            dup2(err_file, STDERR_FILENO);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(cli_path.c_str()));
        for (const auto& arg : args)
            argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        execvp(cli_path.c_str(), argv.data());
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        err = "waitpid failed";
        return -1;
    }
    if (!WIFEXITED(status)) {
        err = "the CLI did not exit";
        return -1;
    }
    return WEXITSTATUS(status);
}

// Remove the per-scenario workspace on every exit path.
struct RemoveOnExit {
    fs::path path;
    ~RemoveOnExit() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

bool prepare_workspace(const Scenario& scenario, const fs::path& ws, std::string& err) {
    write_setup_files(ws, scenario.setup);
    std::error_code ec;
    fs::create_directories(ws / "xdg" / "config", ec);
    fs::create_directories(ws / "xdg" / "state", ec);
    fs::create_directories(ws / "xdg" / "cache", ec);
    if (ec) {
        err = "cannot prepare the workspace: " + ec.message();
        return false;
    }
    return true;
}

std::vector<std::string> cli_args(const Scenario& scenario, const MockServer& server,
                                  const fs::path& ws, const fs::path& telemetry, std::string& err) {
    std::vector<std::string> args = {"--api-base",  server.base_url(), "--model",         "fake",
                                     "--no-stream", "--json-out",      telemetry.string()};
    if (scenario.allow_tools)
        args.emplace_back("--yes");
    if (scenario.turns.empty()) {
        args.emplace_back("--prompt");
        args.push_back(scenario.prompt);
        return args;
    }
    agent::json prompts = agent::json::array();
    for (const auto& turn : scenario.turns)
        prompts.push_back(turn.prompt);
    const fs::path script = ws / "prompts.json";
    std::ofstream out(script);
    if (!out) {
        err = "cannot write the prompt script";
        return {};
    }
    out << prompts.dump();
    args.emplace_back("--script");
    args.push_back(script.string());
    return args;
}

void read_telemetry(const fs::path& path, BinaryE2eResult& result) {
    std::ifstream in(path);
    if (!in) {
        result.error = "no telemetry (exit " + std::to_string(result.exit_code) + ")";
        return;
    }
    try {
        agent::json report;
        in >> report;
        if (report.contains("tool_calls")) {
            for (const auto& call : report["tool_calls"])
                result.actual_tools.emplace_back(call.value("name", ""));
        }
    } catch (const std::exception& e) {
        result.error = std::string("telemetry is not JSON: ") + e.what();
    }
}

void finish_result(BinaryE2eResult& result) {
    result.ok = result.error.empty() && result.exit_code == 0 &&
                result.actual_tools == result.expected_tools;
    if (!result.ok && result.error.empty())
        result.error = "exit " + std::to_string(result.exit_code) + ", tool sequence differs";
}

BinaryE2eResult run_one_binary(const Scenario& scenario, const std::string& cli_path) {
    BinaryE2eResult result;
    result.name = scenario.name;
    std::string err;
    result.expected_tools = expected_tools(scenario, err);
    if (!err.empty()) {
        result.error = "in-process run: " + err;
        return result;
    }

    const std::string workspace = temp_workspace(err);
    if (workspace.empty()) {
        result.error = err;
        return result;
    }
    const fs::path ws = workspace;
    RemoveOnExit cleanup{ws};
    if (!prepare_workspace(scenario, ws, err)) {
        result.error = err;
        return result;
    }

    MockServer server(replies_for(scenario));
    if (!server.start()) {
        result.error = "mock server did not start";
        return result;
    }
    const fs::path telemetry = ws / "telemetry.json";
    const std::vector<std::string> args = cli_args(scenario, server, ws, telemetry, err);
    if (!err.empty()) {
        result.error = err;
        return result;
    }
    result.exit_code = run_cli(cli_path, args, ws, err);
    if (!err.empty()) {
        result.error = err;
        return result;
    }
    read_telemetry(telemetry, result);
    finish_result(result);
    return result;
}

} // namespace

std::vector<BinaryE2eResult> run_binary_scenarios(const std::vector<Scenario>& scenarios,
                                                  const std::string& cli_path, std::string& err) {
    std::vector<BinaryE2eResult> results;
    if (cli_path.empty()) {
        err = "no amber-cli path (set AMBER_BENCH_CLI or build ./amber-cli)";
        return results;
    }
    if (access(cli_path.c_str(), X_OK) != 0) {
        err = "amber-cli is not executable: " + cli_path;
        return results;
    }
    for (const auto& scenario : scenarios)
        results.emplace_back(run_one_binary(scenario, cli_path));
    return results;
}

} // namespace bench
