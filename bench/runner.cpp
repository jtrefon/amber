
#include "bench/runner.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unistd.h>

#include "agent.h"
#include "agent/bootstrap.h"
#include "agent/compressor.h"
#include "agent/data_path.h"
#include "agent/experience.h"
#include "agent/plugin_runtime.h"
#include "agent/plugins_bundled.h"
#include "agent/tools.h"
#include "agent/workspace.h"
#include "bench/fake.h"
#include "bench/oracle.h"
#include "bench/recorder.h"
#include "bench/report.h"
#include "bench/resources.h"
#include "bench/template.h"

namespace fs = std::filesystem;

namespace bench {

namespace {

using agent::json;

long now_ms() noexcept {
    return static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count());
}

void write_setup_files(const fs::path& ws, const json& setup) {
    if (!setup.contains("files") || !setup["files"].is_object())
        return;
    for (auto it = setup["files"].begin(); it != setup["files"].end(); ++it) {
        fs::path p = ws / it.key();
        fs::create_directories(p.parent_path());
        std::ofstream f(p);
        f << (it.value().is_string() ? it.value().get<std::string>() : it.value().dump());
    }
}

void run_setup_shell(const fs::path& ws, const json& setup) {
    if (!setup.contains("shell") || !setup["shell"].is_array())
        return;
    for (const auto& c : setup["shell"]) {
        if (!c.is_string())
            continue;
        const std::string cmd = "cd \"" + ws.string() + "\" && " + c.get<std::string>();
        const int rc = std::system(cmd.c_str());
        (void)rc;
    }
}

void copy_skeleton(const fs::path& template_dir, const fs::path& ws) {
    const fs::path skel = template_dir / "skeleton";
    if (fs::is_directory(skel)) {
        for (const auto& e : fs::recursive_directory_iterator(skel)) {
            fs::path rel = fs::relative(e.path(), skel);
            if (e.is_directory()) {
                fs::create_directories(ws / rel);
            } else if (e.is_regular_file()) {
                fs::create_directories((ws / rel).parent_path());
                fs::copy_file(e.path(), ws / rel, fs::copy_options::overwrite_existing);
            }
        }
    }
    // Task documents at the template root (TASK.md) are part of the contract.
    for (const auto& e : fs::directory_iterator(template_dir)) {
        if (e.is_regular_file() && e.path().extension() == ".md") {
            fs::copy_file(e.path(), ws / e.path().filename(), fs::copy_options::overwrite_existing);
        }
    }
}

fs::path template_root() {
    return fs::current_path() / "bench" / "scenarios";
}

// Restore the process CWD on scope exit (the env card in the system prompt
// reports getcwd(), so scenarios must run with the workspace as CWD).
struct CwdGuard {
    explicit CwdGuard(const fs::path& dir) : saved_(fs::current_path()) {
        const int rc = ::chdir(dir.string().c_str());
        (void)rc;
    }
    ~CwdGuard() {
        const int rc = ::chdir(saved_.string().c_str());
        (void)rc;
    }
    fs::path saved_;
};

// Restore the workspace root on scope exit. The runner sets the root to each
// scenario's temp workspace; without a restore the root leaks across
// scenarios in a serial run (and into the harness probes), pointing at a
// workspace that is removed at teardown.
struct WorkspaceGuard {
    explicit WorkspaceGuard(const fs::path& dir) : saved_(agent::Workspace::root()) {
        agent::Workspace::set_root(dir.string());
    }
    ~WorkspaceGuard() { agent::Workspace::set_root(saved_); }
    std::string saved_;
};

// --- Scenario run steps -----------------------------------------------------

// The host-side objects a run needs, in construction order: the plugin runtime
// is built from the registry, config and workspace, so it comes last.
struct RunHost {
    agent::Config cfg;
    agent::ToolRegistry registry;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::SubAgentExecutor subagents;
    agent::Workspace workspace;
    agent::HostServices host_services;
    agent::PluginRuntime plugins;

    explicit RunHost(agent::Config c)
        : cfg(std::move(c)), host_services{&jobs, &todos, &subagents, &cfg.cancel_token},
          plugins(registry, cfg, workspace) {}
};

// The agent-side objects a run needs, constructed in dependency order.
struct RunComponents {
    agent::CompressionConfig comp_cfg;
    std::unique_ptr<agent::CompressionGate> gate;
    std::unique_ptr<agent::CompressionStrategy> compressor;
    agent::ExperienceConfig exp_cfg;
    std::unique_ptr<agent::MemoryStore> mem_store;
    std::unique_ptr<agent::MemoryRetriever> retriever;
    Recorder recorder;
    agent::AgentHooks hooks;
    ResourceMeter meter;

    explicit RunComponents(const agent::Config& cfg)
        : comp_cfg(agent::load_compression_config(cfg)),
          gate(agent::make_compression_gate(comp_cfg)),
          compressor(agent::make_compressor(comp_cfg)), exp_cfg(agent::load_experience_config(cfg)),
          mem_store(agent::make_memory_store(exp_cfg)),
          retriever(std::make_unique<agent::MemoryRetriever>(*mem_store)), hooks(recorder.hooks()) {
        // Benchmark approval policy: workspace-confined process tools are always
        // allowed; anything else approval-gated (dangerous bash) is denied —
        // mirroring the headless CLI without --yes.
        hooks.on_approval = [](const std::string& tool, const agent::json&,
                               const std::string&) -> agent::Approval {
            return tool.rfind("process_", 0) == 0 ? agent::Approval::AllowSession
                                                  : agent::Approval::Deny;
        };
    }
};

// The run's configuration. Live mode reads amber.conf and auto-detects the
// server; hermetic mode pins the fake client's settings, re-asserting them after
// amber.conf because that file may have clobbered them.
agent::Config build_run_config(const Scenario& s, const RunOptions& opts) {
    agent::Config cfg;
    if (opts.live) {
        std::ifstream def("amber.conf");
        if (def)
            cfg.load("amber.conf");
        cfg.apply_environment();
        if (!opts.model.empty()) {
            cfg.model = opts.model;
            cfg.model_explicit = true;
        }
        if (opts.temperature >= 0)
            cfg.temperature = opts.temperature;
        if (!opts.thinking.empty())
            cfg.thinking = opts.thinking;
        if (opts.thinking_budget >= 0)
            cfg.thinking_budget = opts.thinking_budget;
        if (cfg.system_prompt_path.empty())
            cfg.system_prompt_path = agent::resolve_data_path("prompts/system.md", nullptr);
        if (cfg.tools_prompt_path.empty())
            cfg.tools_prompt_path = agent::resolve_data_path("prompts/tools.md", nullptr);
        // Mirror the CLI (src/main.cpp): fill model/context from the server when
        // the user did not set them explicitly.
        agent::apply_server_autodetect(cfg);
        return cfg;
    }
    cfg.stream = s.stream;
    cfg.context_size = 4096;
    cfg.model = "fake";
    cfg.detection_loop = s.detection_loop;
    cfg.detection_duplicate = s.detection_duplicate;
    cfg.system_prompt_path = agent::resolve_data_path("prompts/system.md", nullptr);
    cfg.tools_prompt_path = agent::resolve_data_path("prompts/tools.md", nullptr);
    if (fs::is_regular_file("amber.conf"))
        cfg.load("amber.conf");
    // Re-assert hermetic defaults that amber.conf may have clobbered.
    cfg.stream = s.stream;
    cfg.context_size = 4096;
    cfg.model = "fake";
    return cfg;
}

// Install the bundled plugins, validate the requested disables, activate, then
// record the plugin set on the run meta. Returns false (with `err` set) when a
// requested plugin does not exist or nothing installed a tool.
bool setup_plugins(const RunOptions& opts, agent::PluginRuntime& plugins,
                   const agent::ToolRegistry& registry, RunMeta& meta, std::string& err) {
    plugins.add_bundled();
    for (const auto& id : opts.disable_plugins) {
        if (!plugins.has(id)) {
            // A typo must not produce a run that quietly measured the default
            // configuration instead of the one that was asked for.
            err = "unknown plugin: " + id;
            return false;
        }
    }
    // The shipped configuration, not this machine's saved plugin state: a result
    // has to mean the same thing on the next machine that runs it.
    plugins.start(/*use_persisted_state=*/false);
    // Disables come after activation, the same order a user's toggle has, and
    // the only order in which they survive: activating a plugin is what turns it
    // on, so a disable applied before it would simply be undone. Applied, not
    // persisted: a measurement is not a preference.
    for (const auto& id : opts.disable_plugins)
        plugins.apply_state(id, false);
    if (registry.empty()) {
        err = "no tools were installed - the plugin set is broken";
        return false;
    }
    if (meta.plugins.empty()) {
        // Recorded once per run, because the plugin set follows from the run
        // options: two scorecards are only comparable when they say which
        // plugins produced them.
        for (const auto& status : plugins.list())
            meta.plugins.push_back({status.id, status.enabled, status.tier});
    }
    return true;
}

// The scenario's step and wall-clock budgets, plus its debug log destinations.
void apply_run_budgets(const Scenario& s, const RunOptions& opts, agent::Config& cfg) {
    // Enforce the scenario step budget during the run (the engine's own iteration
    // cap), not just in post-hoc scoring.
    if (s.max_steps > 0 && s.max_steps < cfg.max_tool_iterations)
        cfg.max_tool_iterations = s.max_steps;
    // Same for the wall-clock budget: the engine must stop at the deadline.
    if (s.max_wall_ms > 0)
        cfg.max_wall_ms = s.max_wall_ms;
    if (!opts.debug_dir.empty()) {
        fs::create_directories(opts.debug_dir);
        cfg.debug_log = (fs::path(opts.debug_dir) / (s.name + ".wire.log")).string();
        cfg.log_path = (fs::path(opts.debug_dir) / (s.name + ".jsonl")).string();
    }
}

// One scripted reply from a scenario's fake_replies entry.
BenchReply bench_reply_from(const json& e) {
    BenchReply r;
    r.content = e.value("content", "");
    if (e.contains("tool_calls"))
        r.tool_calls = e["tool_calls"];
    r.error = e.value("error", "");
    r.retryable = e.value("retryable", true);
    r.latency_ms = e.value("latency_ms", 0L);
    r.drop_after_chunks = e.value("drop_after_chunks", 0);
    r.prompt_tokens = e.value("prompt_tokens", 0L);
    r.completion_tokens = e.value("completion_tokens", 0L);
    return r;
}

// The hermetic fake client, with any scripted sub-agent replies wired into the
// executor. Returns nullptr in live mode, where the real client is used.
std::unique_ptr<agent::LLMClient> build_client(const Scenario& s, const RunOptions& opts,
                                               agent::SubAgentExecutor& subagents) {
    if (opts.live)
        return nullptr;
    // Shared script: the parent and any sub-agents (task tool) each take a copy
    // at construction time, so hermetic runs stay deterministic and never touch
    // the network.
    auto script = std::make_shared<std::deque<BenchReply>>();
    for (const auto& e : s.fake_replies)
        if (e.is_object())
            script->push_back(bench_reply_from(e));
    // A scenario that scripts sub-agent replies is a scenario that delegates;
    // that is now the only signal, since the tool's presence is plugin state
    // rather than a per-scenario switch.
    if (!s.subagent_replies.empty()) {
        auto sub_scripts = std::make_shared<std::vector<std::deque<BenchReply>>>();
        for (const auto& ss : s.subagent_replies) {
            std::deque<BenchReply> dq;
            for (const auto& e : ss)
                if (e.is_object()) {
                    BenchReply r = bench_reply_from(e);
                    r.error.clear(); // sub-agent scripts carry only content/tool_calls
                    dq.push_back(std::move(r));
                }
            sub_scripts->push_back(std::move(dq));
        }
        auto counter = std::make_shared<std::atomic<size_t>>(0);
        subagents.set_factory([sub_scripts, counter](const agent::Config&) {
            auto f = std::make_unique<FakeClient>();
            const size_t i = (*counter)++;
            if (i < sub_scripts->size())
                f->script = (*sub_scripts)[i];
            return std::unique_ptr<agent::LLMClient>(std::move(f));
        });
    }
    auto fake = std::make_unique<FakeClient>();
    fake->script = *script;
    return fake;
}

// Run the agent with the workspace as CWD (the env card advertises getcwd()).
// Returns false and fills `error` when the run threw.
bool run_agent_in_workspace(const Scenario& s, agent::Config& cfg, agent::ToolRegistry& registry,
                            RunComponents& comps, std::unique_ptr<agent::LLMClient> client,
                            const fs::path& ws, std::string& final_text, long& wall_ms, long& t0,
                            std::string& error) {
    try {
        if (!cfg.system_prompt_path.empty())
            cfg.system_prompt_path = fs::absolute(cfg.system_prompt_path).string();
        if (!cfg.tools_prompt_path.empty())
            cfg.tools_prompt_path = fs::absolute(cfg.tools_prompt_path).string();
        CwdGuard cwd(ws);
        agent::Agent agent(cfg, registry, comps.hooks, std::move(comps.compressor),
                           std::move(comps.gate), std::move(comps.mem_store),
                           std::move(comps.retriever), std::move(client));
        agent.policy().init(agent::Workspace::local_dir() + "/policy.json");
        t0 = now_ms();
        final_text = agent.run(s.prompt);
        wall_ms = now_ms() - t0;
        return true;
    } catch (const std::exception& e) {
        error = std::string("agent run threw: ") + e.what();
        return false;
    }
}

// Where the oracle's last matched call landed, in ms from the run's start.
long bullseye_offset(const OracleResult& oracle, const EventStream& stream, long t0, long wall_ms) {
    if (!oracle.success || oracle.matched_call_indexes.empty())
        return wall_ms;
    const size_t last = oracle.matched_call_indexes.back();
    if (last >= stream.calls.size())
        return wall_ms;
    const long t = stream.calls[last].t_ms - t0;
    return t > 0 ? t : wall_ms;
}

// Fold the run's raw results into the report's score fields. Returns whether the
// scenario's checks passed.
bool fill_scores(ScenarioReport& rep, const Scenario& s, const EventStream& stream, const Kpi& kpi,
                 const std::string& final_text) {
    const bool checks_ok = checks_pass(s.checks, final_text);
    // The scenario's checks and (when it has one) its artifact both have to pass
    // before kpi_success applies its budget tests; it short-circuits on
    // kpi.success, so the preconditions are folded in first.
    const bool artifact_ok =
        s.template_dir.empty() ||
        (kpi.compile_ok && kpi.artifact_score == 1.0 && kpi.behavior_equivalent);
    Kpi gated = kpi;
    gated.success = kpi.success && checks_ok && artifact_ok;
    Kpi scored = gated;
    scored.success = kpi_success(gated, s);
    rep.kpi = scored;

    int forbidden = 0;
    for (const auto& c : stream.calls)
        if (std::find(s.forbidden_tools.begin(), s.forbidden_tools.end(), c.name) !=
            s.forbidden_tools.end())
            ++forbidden;
    const double checks_ratio = adherence(s.checks, final_text);
    // Agentic plan adherence measures the model's tool economy; computed in both
    // modes so the score is uniform (hermetic runs measure the engine's scripted
    // tool discipline, live runs the model's).
    rep.agentic = compute_agentic(stream, scored, s);
    // A scenario without an oracle or optimal_plan has no economy baseline; feed
    // the neutral score rather than dragging the total by 0.20.
    const double agentic_score = rep.agentic.has_plan ? rep.agentic.score : 100.0;
    rep.score = compute_score(scored, s, checks_ratio, forbidden, agentic_score);
    return checks_ok;
}

// The report's tool-call rows, with their arguments truncated for storage.
void fill_tool_calls(ScenarioReport& rep, const EventStream& stream) {
    for (const auto& c : stream.calls) {
        std::string args = c.args.dump();
        if (args.size() > 160) {
            args.resize(157);
            args += "...";
        }
        rep.tool_calls.emplace_back(c.name + " [" + c.status + "]", args);
    }
}

// Per-call telemetry (BENCH-11): pair each recorded call with its result detail
// (status, error text, timeout/denied, duration) so a stored run is a
// post-mortem, not a count.
void fill_tool_details(ScenarioReport& rep, const EventStream& stream) {
    for (size_t i = 0; i < stream.calls.size(); ++i) {
        ScenarioReport::ToolDetail d;
        d.name = stream.calls[i].name;
        d.args = stream.calls[i].args.dump();
        d.status = stream.calls[i].status;
        if (i < stream.tools.size()) {
            const ToolEvent& t = stream.tools[i];
            d.error = t.error;
            d.denied = t.denied;
            d.timeout = t.timeout;
            d.duration_ms = t.duration_ms;
            if (t.timeout)
                d.status = "timeout";
        }
        rep.tool_details.push_back(std::move(d));
    }
}

// Max calls issued in a single step, plus the calls-per-step mean and p95.
// Recorded calls are grouped by the loop iteration they were dispatched in (the
// recorder stamps `step`).
void fill_step_metrics(ScenarioReport& rep, const EventStream& stream) {
    int max_in_step = 0;
    int cur = 0;
    int last = -1;
    std::vector<int> per_step;
    for (const auto& c : stream.calls) {
        if (c.step != last) {
            max_in_step = std::max(max_in_step, cur);
            if (cur > 0)
                per_step.push_back(cur);
            cur = 0;
            last = c.step;
        }
        ++cur;
    }
    max_in_step = std::max(max_in_step, cur);
    if (cur > 0)
        per_step.push_back(cur);
    rep.max_calls_per_step = max_in_step;
    if (per_step.empty())
        return;
    double sum = 0.0;
    for (int v : per_step)
        sum += v;
    rep.calls_per_step_mean = sum / static_cast<double>(per_step.size());
    std::sort(per_step.begin(), per_step.end());
    const auto idx = static_cast<size_t>(0.95 * static_cast<double>(per_step.size() - 1));
    rep.calls_per_step_p95 = static_cast<double>(per_step[idx]);
}

// An ordered oracle's steps matched out of dependency order, or the first
// executed call does not satisfy the first oracle step while the task requires
// that order (write-before-read).
bool has_dependency_violation(const Scenario& s, const OracleResult& oracle,
                              const EventStream& stream) {
    const auto& idxs = oracle.matched_call_indexes;
    for (size_t i = 1; i < idxs.size(); ++i)
        if (idxs[i] < idxs[i - 1])
            return true;
    if (stream.calls.empty() || s.oracle.empty())
        return false;
    return stream.calls[0].name != s.oracle[0].tool && oracle.matched_steps > 0 &&
           oracle.matched_steps < oracle.total_steps;
}

// Plan metrics (BENCH-09): adherence in dependency order, replan adaptation
// after failures, dependency-order violations.
void fill_plan_metrics(ScenarioReport& rep, const Scenario& s, const OracleResult& oracle,
                       const EventStream& stream) {
    const auto& idxs = oracle.matched_call_indexes;
    // Adherence = oracle steps whose match kept a strictly increasing call order
    // (the plan's dependency edges hold).
    if (oracle.total_steps > 0 && !idxs.empty()) {
        size_t ordered = 1;
        for (size_t i = 1; i < idxs.size(); ++i)
            if (idxs[i] > idxs[i - 1])
                ++ordered;
        rep.plan_adherence_ratio = static_cast<double>(ordered) / static_cast<double>(idxs.size());
    }
    // Replan: a failure followed by a different (non-failing) call.
    for (size_t i = 0; i + 1 < stream.tools.size(); ++i)
        if (!stream.tools[i].ok && stream.tools[i + 1].ok &&
            stream.tools[i].name == stream.tools[i + 1].name) {
            rep.replan_adapted = true;
            break;
        }
    // Dependency violation: an ordered oracle's steps matched out of dependency
    // order, or the first executed call does not satisfy the first oracle step
    // while the task requires that order (write-before-read).
    if (oracle.total_steps > 0 && has_dependency_violation(s, oracle, stream))
        rep.dependency_violation = true;
}

// Loop-control metrics: how fast a loop broke, and whether steering actually let
// the run complete (BENCH-09).
void fill_loop_metrics(ScenarioReport& rep, const Kpi& kpi) {
    const std::string& ft = rep.final_text;
    if (ft.find("loop detected") != std::string::npos ||
        ft.find("repeated the same tool call") != std::string::npos ||
        ft.find("repeated itself") != std::string::npos)
        rep.breakout_latency = kpi.steps;
    rep.steer_effective = kpi.recoveries > 0 && kpi.success;
}

// The scenario's failure list.
void fill_failures(ScenarioReport& rep, const Scenario& s, const OracleResult& oracle,
                   bool checks_ok, const Kpi& kpi) {
    if (!oracle.success) {
        std::ostringstream msg;
        msg << "oracle not matched: " << oracle.matched_steps << "/" << oracle.total_steps
            << " steps (bullseye " << oracle.bullseye << ")";
        rep.failures.emplace_back(msg.str());
    }
    if (!checks_ok)
        rep.failures.emplace_back("final answer failed scenario checks");
    if (kpi.hard_stop)
        rep.failures.emplace_back("agent hard-stopped (loop)");
    if (s.max_steps > 0 && kpi.steps > s.max_steps)
        rep.failures.emplace_back("step budget exceeded");
    if (s.max_wall_ms > 0 && kpi.wall_ms > s.max_wall_ms)
        rep.failures.emplace_back("wall-clock budget exceeded");
    if (s.template_dir.empty())
        return;
    if (!kpi.compile_ok)
        rep.failures.emplace_back("artifact failed to compile");
    if (kpi.artifact_score < 1.0)
        rep.failures.emplace_back("hidden tests failed");
    if (!kpi.behavior_equivalent)
        rep.failures.emplace_back("artifact behavior differs from reference");
}

// The scenario's private workspace: a fresh temp dir per (scenario, pid).
fs::path make_scenario_workspace(const Scenario& s) {
    const fs::path ws = fs::temp_directory_path() / ("amber_bench_ws_" + s.name + "_" +
                                                     std::to_string(static_cast<long>(::getpid())));
    fs::remove_all(ws);
    fs::create_directories(ws);
    return ws;
}

// Lay the scenario's files down: setup files, the setup shell, then the template
// skeleton (which may overwrite them, as it always has).
void prepare_workspace(const fs::path& ws, const Scenario& s) {
    write_setup_files(ws, s.setup);
    run_setup_shell(ws, s.setup);
    if (!s.template_dir.empty())
        copy_skeleton(template_root() / s.template_dir, ws);
}

// Wire the sub-agent executor and the scenario's step/wall budgets.
void prepare_subagents(const Scenario& s, const RunOptions& opts, RunHost& host) {
    host.subagents.set_config(host.cfg);
    // Single-GPU constraint: the local inference service has ONE slot, and
    // concurrent requests pay a long prefill penalty. Bench runs must never fire
    // parallel LLM requests — sub-agents (task tool) run serially.
    host.subagents.set_parallel(false);
    host.subagents.set_max(host.cfg.subagent_max);
    apply_run_budgets(s, opts, host.cfg);
}

// Score the finished run and fill every report field from it.
void fill_report(ScenarioReport& rep, const Scenario& s, const agent::Config& cfg,
                 RunComponents& comps, const std::string& final_text, long wall_ms, long t0,
                 const fs::path& ws) {
    const EventStream& stream = comps.recorder.stream();
    const OracleResult oracle = score_oracle(s.oracle, stream.calls);
    TemplateResult tmpl;
    if (!s.template_dir.empty()) {
        std::string terr;
        tmpl = run_template(fs::absolute(template_root() / s.template_dir).string(), ws.string(),
                            "g++", terr);
        if (!terr.empty())
            rep.failures.emplace_back("template: " + terr);
    }
    const Kpi kpi = compute_kpi(stream, oracle, comps.meter, tmpl, s.prompt_checks, final_text,
                                wall_ms, bullseye_offset(oracle, stream, t0, wall_ms));
    const bool checks_ok = fill_scores(rep, s, stream, kpi, final_text);

    rep.final_text = final_text;
    rep.templated = !s.template_dir.empty();
    rep.difficulty = s.difficulty;
    rep.reasoning = cfg.thinking;
    rep.total_steps = rep.kpi.steps;
    fill_tool_calls(rep, stream);
    fill_tool_details(rep, stream);
    fill_step_metrics(rep, stream);
    fill_plan_metrics(rep, s, oracle, stream);
    fill_loop_metrics(rep, rep.kpi);
    fill_failures(rep, s, oracle, checks_ok, rep.kpi);
}

} // namespace

ScenarioReport run_one_scenario(const Scenario& s, const RunOptions& opts, RunMeta& meta,
                                std::string& err) {
    ScenarioReport rep;
    rep.name = s.name;
    rep.suite = s.suite;
    err.clear();
    if (!platform_supported(s)) {
        rep.failures.emplace_back("platform not supported by this scenario");
        return rep;
    }

    const fs::path ws = make_scenario_workspace(s);
    WorkspaceGuard ws_guard(ws);
    prepare_workspace(ws, s);

    // The tools arrive through the plugin runtime, exactly as they do in every
    // host: the harness measures what a user runs, not a parallel installation
    // path. Plugin state comes with it, which is what makes "run the suite with
    // tool_search off" a real experiment rather than a cosmetic flag.
    RunHost host(build_run_config(s, opts));
    host.plugins.attach_host_services(host.host_services);
    host.plugins.attach_config(host.cfg);
    if (!setup_plugins(opts, host.plugins, host.registry, meta, err))
        return {};
    prepare_subagents(s, opts, host);

    RunComponents comps(host.cfg);
    comps.meter.start();
    std::unique_ptr<agent::LLMClient> client = build_client(s, opts, host.subagents);

    std::string final_text;
    long wall_ms = 0;
    long t0 = 0;
    if (!run_agent_in_workspace(s, host.cfg, host.registry, comps, std::move(client), ws,
                                final_text, wall_ms, t0, err)) {
        comps.meter.stop();
        rep.failures.emplace_back(err);
        fs::remove_all(ws);
        return rep;
    }
    comps.meter.stop();

    fill_report(rep, s, host.cfg, comps, final_text, wall_ms, t0, ws);
    fs::remove_all(ws);
    return rep;
}

std::vector<ScenarioReport> run_scenarios(const std::vector<Scenario>& scenarios,
                                          const RunOptions& opts, RunMeta& meta) {
    std::vector<ScenarioReport> out;
    for (const auto& s : scenarios) {
        if (s.hermetic_only && opts.live)
            continue;
        if (!opts.live && s.fake_replies.empty())
            continue;
        std::string err;
        std::vector<ScenarioReport> runs;
        for (int i = 0; i < std::max(1, opts.repeat); ++i) {
            ScenarioReport rep = run_one_scenario(s, opts, meta, err);
            if (rep.name.empty()) {
                // The scenario could not be set up. Name it and carry the
                // reason, so a run that measured nothing says what it was and
                // why - an anonymous zero would be worse than useless.
                rep.name = s.name;
                rep.suite = s.suite;
                if (!err.empty())
                    rep.failures.emplace_back(err);
            }
            runs.emplace_back(std::move(rep));
        }
        // With --repeat N the report is the median run plus the population
        // statistics (BENCH-01): model scores then aggregate medians, and the
        // confidence interval gives the resolution floor.
        out.push_back(aggregate_repeats(runs));
    }
    return out;
}

} // namespace bench
