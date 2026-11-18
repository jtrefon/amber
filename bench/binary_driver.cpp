#include "bench/binary_driver.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <system_error>

#include "bench/client_filters.h"
#include "bench/report.h"
#include "bench/runner.h"
#include "bench/trace_store.h"
#include "bench/workspace_check.h"

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

std::deque<EndpointReply> replies_for(const Scenario& scenario) {
    std::deque<EndpointReply> replies;
    const agent::json& script =
        scenario.endpoint_replies.empty() ? scenario.fake_replies : scenario.endpoint_replies;
    for (const auto& entry : script) {
        if (!entry.is_object())
            continue;
        EndpointReply reply;
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

// The in-process run the CLI must reproduce: the tool names, and whether each
// call succeeded. Denied and timed-out calls are failures, exactly as the CLI
// reports them (`ok` vs `error`).
struct ExpectedRun {
    std::vector<std::string> tools;
    std::vector<bool> ok;
};

ExpectedRun expected_run(const Scenario& scenario, std::string& err) {
    RunOptions opts;
    RunMeta meta;
    meta.mode = "hermetic";
    meta.model = "fake";
    const ScenarioReport report = run_one_scenario(scenario, opts, meta, err);
    ExpectedRun out;
    for (const auto& detail : report.tool_details) {
        out.tools.push_back(detail.name);
        out.ok.push_back(detail.status == "ok");
    }
    if (out.tools.empty()) {
        for (const auto& call : report.tool_calls) {
            out.tools.push_back(call.first);
            out.ok.push_back(true);
        }
    }
    return out;
}

bool file_contains(const fs::path& path, const std::string& marker) {
    std::ifstream in(path);
    if (!in)
        return false;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return text.find(marker) != std::string::npos;
}

// Run one CLI process. `cancel_marker` makes the driver wait for that text on
// stderr and then send SIGINT, which is how the cancel journey reaches the
// normal cancellation path instead of killing the process. The deadline is the
// scenario's wall budget plus slack: a stuck CLI fails with a message rather
// than hanging the suite.
pid_t spawn_cli(const std::string& cli_path, const std::vector<std::string>& args,
                const fs::path& ws) {
    const pid_t pid = fork();
    if (pid < 0)
        return -1;
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
    return pid;
}

// Poll until the process exits. The cancel marker turns the wait into the
// cancel journey's SIGINT; the deadline turns a wedged CLI into a named failure.
int wait_for_cli(pid_t pid, const fs::path& ws, const std::string& cancel_marker, long timeout_ms,
                 std::string& err) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    bool cancelled = false;
    int status = 0;
    for (;;) {
        const pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid)
            break;
        if (done < 0) {
            err = "waitpid failed";
            return -1;
        }
        if (!cancelled && !cancel_marker.empty() && file_contains(ws / "err.txt", cancel_marker)) {
            kill(pid, SIGINT);
            cancelled = true;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            err = "the CLI did not exit within " + std::to_string(timeout_ms) + "ms";
            return -1;
        }
        usleep(20000);
    }
    if (!WIFEXITED(status)) {
        err = "the CLI did not exit";
        return -1;
    }
    return WEXITSTATUS(status);
}

int run_cli(const std::string& cli_path, const std::vector<std::string>& args, const fs::path& ws,
            const std::string& cancel_marker, long timeout_ms, std::string& err) {
    const pid_t pid = spawn_cli(cli_path, args, ws);
    if (pid < 0) {
        err = "fork failed";
        return -1;
    }
    return wait_for_cli(pid, ws, cancel_marker, timeout_ms, err);
}

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

// One CLI journey: the scenario, its in-process reference run and the options
// that apply to it.
struct BinaryJourney {
    const Scenario* scenario = nullptr;
    ExpectedRun expected;
    BinaryE2eOptions options;
};

struct CliInvocation {
    std::string prompt; // empty -> --script from the scenario turns
    std::string session;
    fs::path telemetry;
};

void append_scenario_config(const Scenario& scenario, const fs::path& ws,
                            std::vector<std::string>& args) {
    if (scenario.compression_threshold < 0 && scenario.compression_min_turns < 0 &&
        scenario.compression_cooldown_turns < 0 && scenario.compression_target_pct < 0 &&
        scenario.context_size <= 0) {
        return;
    }
    const fs::path config = ws / "scenario.conf";
    std::ofstream out(config);
    if (scenario.context_size > 0)
        out << "context_size=" << scenario.context_size << "\n";
    if (scenario.compression_threshold >= 0)
        out << "compression_threshold=" << scenario.compression_threshold << "\n";
    if (scenario.compression_min_turns >= 0)
        out << "compression_min_turns=" << scenario.compression_min_turns << "\n";
    if (scenario.compression_cooldown_turns >= 0)
        out << "compression_cooldown_turns=" << scenario.compression_cooldown_turns << "\n";
    if (scenario.compression_target_pct >= 0)
        out << "compression_target_pct=" << scenario.compression_target_pct << "\n";
    out.close();
    args.emplace_back("--config");
    args.push_back(config.string());
}

bool append_prompt_args(const Scenario& scenario, const fs::path& ws,
                        const CliInvocation& invocation, std::vector<std::string>& args,
                        std::string& err) {
    if (!invocation.prompt.empty()) {
        args.emplace_back("--prompt");
        args.push_back(invocation.prompt);
        return true;
    }
    agent::json prompts = agent::json::array();
    for (const auto& turn : scenario.turns)
        prompts.push_back(turn.prompt);
    const fs::path script = ws / "prompts.json";
    std::ofstream out(script);
    if (!out) {
        err = "cannot write the prompt script";
        return false;
    }
    out << prompts.dump();
    args.emplace_back("--script");
    args.push_back(script.string());
    return true;
}

std::vector<std::string> cli_args(const Scenario& scenario, const std::string& api_base,
                                  const std::string& model, const fs::path& ws,
                                  const CliInvocation& invocation, std::string& err) {
    std::vector<std::string> args = {"--api-base",
                                     api_base,
                                     "--model",
                                     model,
                                     "--no-stream",
                                     "--json-out",
                                     invocation.telemetry.string()};
    if (scenario.allow_tools)
        args.emplace_back("--yes");
    append_scenario_config(scenario, ws, args);
    if (!invocation.session.empty()) {
        args.emplace_back("--session");
        args.push_back(invocation.session);
    }
    if (!append_prompt_args(scenario, ws, invocation, args, err))
        return {};
    return args;
}

struct BinaryTelemetry {
    std::string final_text;
    int exit_code = -1;
    long prompt_tokens = 0;
    long completion_tokens = 0;
    long wall_ms = 0;
    int compactions = 0;
    std::vector<std::pair<std::string, std::string>> tools; // name, ok | error
};

bool read_telemetry(const fs::path& path, int process_exit, BinaryTelemetry& out,
                    std::string& err) {
    std::ifstream in(path);
    if (!in) {
        err = "no telemetry (exit " + std::to_string(process_exit) + ")";
        return false;
    }
    agent::json report;
    try {
        in >> report;
    } catch (const std::exception& e) {
        err = std::string("telemetry is not JSON: ") + e.what();
        return false;
    }
    out.final_text = report.value("final_text", std::string());
    out.exit_code = report.value("exit_code", -1);
    out.wall_ms = report.value("wall_ms", 0L);
    out.compactions = report.value("compactions", 0);
    if (report.contains("usage") && report["usage"].is_object()) {
        out.prompt_tokens = report["usage"].value("prompt_tokens", 0L);
        out.completion_tokens = report["usage"].value("completion_tokens", 0L);
    }
    if (report.contains("tool_calls") && report["tool_calls"].is_array()) {
        for (const auto& call : report["tool_calls"])
            out.tools.emplace_back(call.value("name", ""), call.value("status", ""));
    }
    return true;
}

std::string join_failures(const std::vector<std::string>& failures) {
    std::string out;
    for (const std::string& failure : failures) {
        if (!out.empty())
            out += "; ";
        out += failure;
    }
    return out;
}

void check_exit(const Scenario& scenario, const BinaryE2eResult& result,
                const BinaryTelemetry& telemetry, std::vector<std::string>& failures) {
    if (result.exit_code != scenario.expected_exit_code) {
        failures.push_back("exit code " + std::to_string(scenario.expected_exit_code) + ", got " +
                           std::to_string(result.exit_code));
    }
    if (telemetry.exit_code >= 0 && telemetry.exit_code != result.exit_code) {
        failures.push_back("telemetry reports exit " + std::to_string(telemetry.exit_code) +
                           ", process exited " + std::to_string(result.exit_code));
    }
}

void check_tools(const ExpectedRun& expected, const BinaryTelemetry& telemetry, bool compare_status,
                 std::vector<std::string>& failures) {
    if (telemetry.tools.size() != expected.tools.size()) {
        failures.push_back("tool sequence differs: " + std::to_string(telemetry.tools.size()) +
                           " calls, expected " + std::to_string(expected.tools.size()));
        return;
    }
    for (std::size_t i = 0; i < expected.tools.size(); ++i) {
        if (telemetry.tools[i].first != expected.tools[i]) {
            failures.push_back("tool sequence differs: call " + std::to_string(i + 1) + " is '" +
                               telemetry.tools[i].first + "', expected '" + expected.tools[i] +
                               "'");
            return;
        }
        if (!compare_status)
            continue;
        const bool actual_ok =
            telemetry.tools[i].second.empty() || telemetry.tools[i].second == "ok";
        if (actual_ok != expected.ok[i]) {
            failures.push_back("tool status differs: call " + std::to_string(i + 1) + " ('" +
                               expected.tools[i] + "') is '" + telemetry.tools[i].second + "'");
            return;
        }
    }
}

void check_usage(const Scenario& scenario, const BinaryTelemetry& telemetry,
                 std::vector<std::string>& failures) {
    if (telemetry.prompt_tokens < scenario.min_prompt_tokens) {
        failures.push_back("prompt tokens " + std::to_string(telemetry.prompt_tokens) +
                           " below floor " + std::to_string(scenario.min_prompt_tokens));
    }
    if (telemetry.completion_tokens < scenario.min_completion_tokens) {
        failures.push_back("completion tokens " + std::to_string(telemetry.completion_tokens) +
                           " below floor " + std::to_string(scenario.min_completion_tokens));
    }
    if (telemetry.compactions < scenario.min_compactions) {
        failures.push_back("compactions " + std::to_string(telemetry.compactions) +
                           " below floor " + std::to_string(scenario.min_compactions));
    }
}

// The endpoint the binary talks to: scripted replies or a recorded trace.
struct DriverEndpoint {
    // Declared before the server: the server thread calls into the responder,
    // so it has to stop first (members destruct in reverse order).
    std::unique_ptr<TraceResponder> trace_responder;
    std::unique_ptr<ScriptedEndpoint> server;
    std::string api_base;
    std::string model = "fake";
};

bool start_endpoint(const Scenario& scenario, const BinaryE2eOptions& options, DriverEndpoint& out,
                    std::string& err) {
    if (options.trace_path.empty()) {
        out.server = std::make_unique<ScriptedEndpoint>(replies_for(scenario));
        if (!out.server->start()) {
            err = "mock server did not start";
            return false;
        }
        out.api_base = out.server->base_url();
        return true;
    }
    TraceMode mode = TraceMode::Strict;
    if (!parse_trace_mode(options.trace_mode, mode)) {
        err = "unknown trace mode '" + options.trace_mode + "' (expected strict | shape)";
        return false;
    }
    std::optional<Trace> trace = load_trace(options.trace_path, err);
    if (!trace)
        return false;
    out.trace_responder =
        std::make_unique<TraceResponder>(std::move(*trace), mode, options.trace_path);
    out.server = std::make_unique<ScriptedEndpoint>(
        [responder = out.trace_responder.get()](const EndpointRequest& request) {
            return responder->respond(request);
        });
    if (!out.server->start()) {
        err = "the trace endpoint did not start";
        return false;
    }
    out.api_base = out.server->base_url();
    out.model = "trace";
    return true;
}

void check_request_log_contains(const Scenario& scenario, const DriverEndpoint& endpoint,
                                std::vector<std::string>& failures) {
    if (scenario.request_log_contains.empty())
        return;
    std::vector<std::string> bodies;
    if (endpoint.server) {
        for (const EndpointRequest& request : endpoint.server->requests())
            if (endpoint_is_chat_path(request.path))
                bodies.push_back(request.body);
    }
    for (const std::string& wanted : scenario.request_log_contains) {
        bool found = false;
        for (const std::string& body : bodies) {
            if (body.find(wanted) != std::string::npos) {
                found = true;
                break;
            }
        }
        if (!found)
            failures.push_back("request log does not contain \"" + wanted + "\"");
    }
}

void retain_requests(const fs::path& ws, const DriverEndpoint& endpoint) {
    agent::json log = agent::json::array();
    if (endpoint.server) {
        for (const EndpointRequest& request : endpoint.server->requests())
            log.push_back({{"path", request.path}, {"body", request.json}});
    }
    std::ofstream out(ws / "requests.json");
    out << log.dump(2) << "\n";
}

void collect_failures(BinaryE2eResult& result, const BinaryJourney& journey,
                      const BinaryTelemetry& telemetry, const fs::path& ws) {
    const Scenario& scenario = *journey.scenario;
    check_exit(scenario, result, telemetry, result.failures);
    if (!checks_pass(scenario.checks, telemetry.final_text))
        result.failures.emplace_back("final answer failed the scenario checks");
    const std::vector<std::string> workspace = check_expected_workspace(scenario, ws);
    result.failures.insert(result.failures.end(), workspace.begin(), workspace.end());
    const std::vector<std::string> cli_workspace =
        check_workspace_block(scenario.cli_expected_workspace, scenario.setup, ws);
    result.failures.insert(result.failures.end(), cli_workspace.begin(), cli_workspace.end());
    check_usage(scenario, telemetry, result.failures);
    check_tools(journey.expected, telemetry, scenario.cancel_marker.empty(), result.failures);
}

void finish_binary_run(BinaryE2eResult& result, const BinaryJourney& journey,
                       DriverEndpoint& endpoint, const fs::path& ws,
                       const BinaryTelemetry& telemetry) {
    result.final_text = telemetry.final_text;
    result.prompt_tokens = telemetry.prompt_tokens;
    result.completion_tokens = telemetry.completion_tokens;
    result.wall_ms = telemetry.wall_ms;
    result.actual_tools.clear();
    for (const auto& tool : telemetry.tools)
        result.actual_tools.push_back(tool.first);
    collect_failures(result, journey, telemetry, ws);
    check_request_log_contains(*journey.scenario, endpoint, result.failures);
    if (endpoint.trace_responder) {
        std::string trace_err;
        if (!endpoint.trace_responder->finish(trace_err))
            result.failures.push_back(trace_err);
    }
    std::string contract_err;
    if (endpoint.server && !check_request_log(journey.scenario->request_contract,
                                              endpoint.server->requests(), contract_err))
        result.failures.push_back(contract_err);
    result.ok = result.failures.empty();
    result.error = join_failures(result.failures);
    if (!result.ok && journey.options.keep_failures) {
        retain_requests(ws, endpoint);
        result.workspace = ws.string();
        return;
    }
    std::error_code ec;
    fs::remove_all(ws, ec);
}

// The invocations a scenario needs: one process, or one per turn when `resume`
// makes the saved session the boundary between them.
std::vector<CliInvocation> invocations_for(const Scenario& scenario, const fs::path& ws) {
    std::vector<CliInvocation> out;
    if (!scenario.resume) {
        // Turns without --resume are one process driven by --script; the
        // top-level prompt is only for a one-shot scenario.
        const std::string prompt = scenario.turns.empty() ? scenario.prompt : std::string();
        out.push_back({prompt, scenario.session, ws / "telemetry.json"});
        return out;
    }
    for (std::size_t i = 0; i < scenario.turns.size(); ++i)
        out.push_back({scenario.turns[i].prompt, scenario.session,
                       ws / ("telemetry-" + std::to_string(i) + ".json")});
    return out;
}

// Point the binary at the endpoint, run it (once or per resumed turn), and
// merge what it reported.
bool run_binary_agent(const BinaryJourney& journey, const std::string& cli_path, const fs::path& ws,
                      BinaryE2eResult& result, std::string& err) {
    DriverEndpoint endpoint;
    if (!start_endpoint(*journey.scenario, journey.options, endpoint, err))
        return false;
    const long timeout_ms =
        journey.scenario->max_wall_ms > 0 ? journey.scenario->max_wall_ms + 30000 : 120000;
    BinaryTelemetry aggregate;
    for (const CliInvocation& invocation : invocations_for(*journey.scenario, ws)) {
        const std::vector<std::string> args =
            cli_args(*journey.scenario, endpoint.api_base, endpoint.model, ws, invocation, err);
        if (!err.empty())
            return false;
        result.exit_code =
            run_cli(cli_path, args, ws, journey.scenario->cancel_marker, timeout_ms, err);
        if (!err.empty())
            return false;
        BinaryTelemetry one;
        if (!read_telemetry(invocation.telemetry, result.exit_code, one, err))
            return false;
        aggregate.final_text = one.final_text;
        aggregate.prompt_tokens += one.prompt_tokens;
        aggregate.completion_tokens += one.completion_tokens;
        aggregate.wall_ms += one.wall_ms;
        aggregate.compactions += one.compactions;
        aggregate.tools.insert(aggregate.tools.end(), one.tools.begin(), one.tools.end());
    }
    finish_binary_run(result, journey, endpoint, ws, aggregate);
    return true;
}

BinaryE2eResult run_one_binary(const Scenario& scenario, const std::string& cli_path,
                               const BinaryE2eOptions& options) {
    BinaryE2eResult result;
    result.name = scenario.name;
    std::string err;
    BinaryJourney journey;
    journey.scenario = &scenario;
    journey.options = options;
    journey.expected = expected_run(scenario, err);
    if (!scenario.expected_tools.empty()) {
        journey.expected.tools = scenario.expected_tools;
        journey.expected.ok.assign(scenario.expected_tools.size(), true);
    }
    result.expected_tools = journey.expected.tools;
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
    if (!prepare_workspace(scenario, ws, err)) {
        std::error_code ec;
        fs::remove_all(ws, ec);
        result.error = err;
        return result;
    }
    if (!run_binary_agent(journey, cli_path, ws, result, err)) {
        std::error_code ec;
        fs::remove_all(ws, ec);
        result.error = err;
        return result;
    }
    return result;
}

} // namespace

std::string resolve_cli_path(const std::string& exe_path, const std::string& cwd) {
    const auto executable = [](const fs::path& path) { return access(path.c_str(), X_OK) == 0; };
    if (!exe_path.empty()) {
        const fs::path beside = fs::path(exe_path).parent_path() / "amber-cli";
        if (executable(beside))
            return beside.string();
    }
    const fs::path checkout = fs::path(cwd) / "amber-cli";
    return executable(checkout) ? checkout.string() : std::string();
}

std::vector<BinaryE2eResult> run_binary_scenarios(const std::vector<Scenario>& scenarios,
                                                  const std::string& cli_path, std::string& err) {
    return run_binary_scenarios(scenarios, cli_path, BinaryE2eOptions{}, err);
}

std::vector<BinaryE2eResult> run_binary_scenarios(const std::vector<Scenario>& scenarios,
                                                  const std::string& cli_path,
                                                  const BinaryE2eOptions& options,
                                                  std::string& err) {
    std::vector<BinaryE2eResult> results;
    if (cli_path.empty()) {
        err = "no amber-cli to run (build it beside amber-bench or in the working directory)";
        return results;
    }
    if (access(cli_path.c_str(), X_OK) != 0) {
        err = "amber-cli is not executable: " + cli_path;
        return results;
    }
    for (const auto& scenario : scenarios)
        results.emplace_back(run_one_binary(scenario, cli_path, options));
    return results;
}

} // namespace bench
