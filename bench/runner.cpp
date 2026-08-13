
#include "bench/runner.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

#include "agent.h"
#include "agent/bootstrap.h"
#include "agent/compressor.h"
#include "agent/data_path.h"
#include "agent/experience.h"
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
    return static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void write_setup_files(const fs::path& ws, const json& setup) {
    if (!setup.contains("files") || !setup["files"].is_object()) return;
    for (auto it = setup["files"].begin(); it != setup["files"].end(); ++it) {
        fs::path p = ws / it.key();
        fs::create_directories(p.parent_path());
        std::ofstream f(p);
        f << (it.value().is_string() ? it.value().get<std::string>()
                                     : it.value().dump());
    }
}

void run_setup_shell(const fs::path& ws, const json& setup) {
    if (!setup.contains("shell") || !setup["shell"].is_array()) return;
    for (const auto& c : setup["shell"]) {
        if (!c.is_string()) continue;
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
                fs::copy_file(e.path(), ws / rel,
                              fs::copy_options::overwrite_existing);
            }
        }
    }
    // Task documents at the template root (TASK.md) are part of the contract.
    for (const auto& e : fs::directory_iterator(template_dir)) {
        if (e.is_regular_file() && e.path().extension() == ".md") {
            fs::copy_file(e.path(), ws / e.path().filename(),
                          fs::copy_options::overwrite_existing);
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
    explicit WorkspaceGuard(const fs::path& dir)
        : saved_(agent::Workspace::root()) {
        agent::Workspace::set_root(dir.string());
    }
    ~WorkspaceGuard() { agent::Workspace::set_root(saved_); }
    std::string saved_;
};

} // namespace

ScenarioReport run_one_scenario(const Scenario& s, const RunOptions& opts,
                                const RunMeta& meta, std::string& err) {
    (void)meta;
    ScenarioReport rep;
    rep.name = s.name;
    rep.suite = s.suite;
    err.clear();
    if (!platform_supported(s)) {
        rep.failures.emplace_back("platform not supported by this scenario");
        return rep;
    }

    const fs::path ws = fs::temp_directory_path() /
                        ("amber_bench_ws_" + s.name + "_" +
                         std::to_string(static_cast<long>(::getpid())));
    fs::remove_all(ws);
    fs::create_directories(ws);
    WorkspaceGuard ws_guard(ws);

    const fs::path tpl_abs =
        s.template_dir.empty()
            ? fs::path()
            : fs::absolute(template_root() / s.template_dir);

    write_setup_files(ws, s.setup);
    run_setup_shell(ws, s.setup);
    if (!s.template_dir.empty())
        copy_skeleton(template_root() / s.template_dir, ws);

    agent::Config cfg;
    if (opts.live) {
        std::ifstream def("amber.conf");
        if (def) cfg.load("amber.conf");
        cfg.apply_environment();
        if (!opts.model.empty()) {
            cfg.model = opts.model;
            cfg.model_explicit = true;
        }
        if (opts.temperature >= 0) cfg.temperature = opts.temperature;
        if (!opts.thinking.empty()) cfg.thinking = opts.thinking;
        if (opts.thinking_budget >= 0) cfg.thinking_budget = opts.thinking_budget;
        if (cfg.system_prompt_path.empty())
            cfg.system_prompt_path =
                agent::resolve_data_path("prompts/system.md", nullptr);
        if (cfg.tools_prompt_path.empty())
            cfg.tools_prompt_path =
                agent::resolve_data_path("prompts/tools.md", nullptr);
        // Mirror the CLI (src/main.cpp): fill model/context from the server
        // when the user did not set them explicitly.
        agent::apply_server_autodetect(cfg);
    } else {
        cfg.stream = s.stream;
        cfg.context_size = 4096;
        cfg.model = "fake";
        cfg.detection_loop = s.detection_loop;
        cfg.detection_duplicate = s.detection_duplicate;
        cfg.system_prompt_path =
            agent::resolve_data_path("prompts/system.md", nullptr);
        cfg.tools_prompt_path =
            agent::resolve_data_path("prompts/tools.md", nullptr);
        if (fs::is_regular_file("amber.conf")) cfg.load("amber.conf");
        // Re-assert hermetic defaults that amber.conf may have clobbered.
        cfg.stream = s.stream;
        cfg.context_size = 4096;
        cfg.model = "fake";
    }

    // Scenario-level opt-in: a delegation scenario may enable the task tool
    // even when the default config keeps it out of the schema.
    cfg.task_tool = cfg.task_tool || s.task_tool;

    agent::ToolRegistry registry;
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::SubAgentExecutor subagents;
    agent::register_default_tools(registry, jobs, todos, cfg.cancel_token,
                        cfg.plan_tool, subagents, cfg.task_tool);
    subagents.set_config(cfg);
    // Single-GPU constraint: the local inference service has ONE slot, and
    // concurrent requests pay a long prefill penalty. Bench runs must never
    // fire parallel LLM requests — sub-agents (task tool) run serially.
    subagents.set_parallel(false);
    subagents.set_max(cfg.subagent_max);

    // Enforce the scenario step budget during the run (the engine's own
    // iteration cap), not just in post-hoc scoring.
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

    // Benchmark approval policy: workspace-confined process tools are always
    // allowed; anything else approval-gated (dangerous bash) is denied —
    // mirroring the headless CLI without --yes.
    auto comp_cfg = agent::load_compression_config(cfg);
    auto gate = agent::make_compression_gate(comp_cfg);
    auto compressor = agent::make_compressor(comp_cfg);
    auto exp_cfg = agent::load_experience_config(cfg);
    auto mem_store = agent::make_memory_store(exp_cfg);
    auto retriever =
        std::make_unique<agent::MemoryRetriever>(*mem_store);

    Recorder recorder;
    agent::AgentHooks hooks = recorder.hooks();
    hooks.on_approval =
        [](const std::string& tool, const agent::json&,
           const std::string&) -> agent::Approval {
        return tool.rfind("process_", 0) == 0 ? agent::Approval::AllowSession
                                              : agent::Approval::Deny;
    };
    ResourceMeter meter;
    meter.start();

    std::unique_ptr<agent::LLMClient> client;
    if (!opts.live) {
        // Shared script: the parent and any sub-agents (task tool) each take
        // a copy at construction time, so hermetic runs stay deterministic
        // and never touch the network.
        auto script = std::make_shared<std::deque<BenchReply>>();
        for (const auto& e : s.fake_replies) {
            if (!e.is_object()) continue;
            BenchReply r;
            r.content = e.value("content", "");
            if (e.contains("tool_calls")) r.tool_calls = e["tool_calls"];
            r.error = e.value("error", "");
            r.retryable = e.value("retryable", true);
            r.latency_ms = e.value("latency_ms", 0L);
            r.drop_after_chunks = e.value("drop_after_chunks", 0);
            r.prompt_tokens = e.value("prompt_tokens", 0L);
            r.completion_tokens = e.value("completion_tokens", 0L);
            script->push_back(std::move(r));
        }
        if (s.task_tool) {
            auto sub_scripts =
                std::make_shared<std::vector<std::deque<BenchReply>>>();
            for (const auto& ss : s.subagent_replies) {
                std::deque<BenchReply> dq;
                for (const auto& e : ss) {
                    if (!e.is_object()) continue;
                    BenchReply r;
                    r.content = e.value("content", "");
                    if (e.contains("tool_calls")) r.tool_calls = e["tool_calls"];
                    dq.push_back(std::move(r));
                }
                sub_scripts->push_back(std::move(dq));
            }
            auto counter = std::make_shared<std::atomic<size_t>>(0);
            subagents.set_factory(
                [sub_scripts, counter](const agent::Config&) {
                    auto f = std::make_unique<FakeClient>();
                    const size_t i = (*counter)++;
                    if (i < sub_scripts->size())
                        f->script = (*sub_scripts)[i];
                    return std::unique_ptr<agent::LLMClient>(std::move(f));
                });
        }
        auto fake = std::make_unique<FakeClient>();
        fake->script = *script;
        client = std::move(fake);
    }

    std::string final_text;
    long wall_ms = 0;
    long t0 = 0;
    try {
        // The env card advertises getcwd() as the working directory — run
        // with the workspace as CWD so the model sees the scenario files.
        if (!cfg.system_prompt_path.empty())
            cfg.system_prompt_path = fs::absolute(cfg.system_prompt_path).string();
        if (!cfg.tools_prompt_path.empty())
            cfg.tools_prompt_path = fs::absolute(cfg.tools_prompt_path).string();
        CwdGuard cwd(ws);
        agent::Agent agent(cfg, registry, hooks,
                           std::move(compressor), std::move(gate),
                           std::move(mem_store), std::move(retriever),
                           std::move(client));
        agent.policy().init(agent::Workspace::local_dir() + "/policy.json");
        t0 = now_ms();
        final_text = agent.run(s.prompt);
        wall_ms = now_ms() - t0;
    } catch (const std::exception& e) {
        meter.stop();
        rep.failures.emplace_back(std::string("agent run threw: ") + e.what());
        fs::remove_all(ws);
        return rep;
    }
    meter.stop();

    OracleResult oracle = score_oracle(s.oracle, recorder.stream().calls);

    TemplateResult tmpl;
    if (!s.template_dir.empty()) {
        std::string terr;
        tmpl = run_template(tpl_abs.string(), ws.string(), "g++", terr);
        if (!terr.empty()) rep.failures.emplace_back("template: " + terr);
    }

    long bullseye_at = wall_ms;
    if (oracle.success && !oracle.matched_call_indexes.empty()) {
        const size_t last = oracle.matched_call_indexes.back();
        if (last < recorder.stream().calls.size()) {
            const long t = recorder.stream().calls[last].t_ms - t0;
            if (t > 0) bullseye_at = t;
        }
    }

    Kpi kpi = compute_kpi(recorder.stream(), oracle, meter, tmpl,
                          s.prompt_checks, final_text, wall_ms, bullseye_at);
    const bool checks_ok = checks_pass(s.checks, final_text);
    if (!checks_ok) kpi.success = false;
    if (!s.template_dir.empty() &&
        (!kpi.compile_ok || kpi.artifact_score != 1.0 ||
         !kpi.behavior_equivalent))
        kpi.success = false;
    kpi.success = kpi_success(kpi, s);
    rep.kpi = kpi;
    rep.final_text = final_text;
    rep.templated = !s.template_dir.empty();
    rep.difficulty = s.difficulty;
    rep.reasoning = cfg.thinking;

    int forbidden = 0;
    for (const auto& c : recorder.stream().calls)
        if (std::find(s.forbidden_tools.begin(), s.forbidden_tools.end(),
                      c.name) != s.forbidden_tools.end())
            ++forbidden;
    const double checks_ratio = adherence(s.checks, final_text);
    // Agentic plan adherence measures the model's tool economy; computed in
    // both modes so the score is uniform (hermetic runs measure the engine's
    // scripted tool discipline, live runs the model's).
    rep.agentic = compute_agentic(recorder.stream(), kpi, s);
    // A scenario without an oracle or optimal_plan has no economy baseline;
    // feed the neutral score rather than dragging the total by 0.20.
    const double agentic_score =
        rep.agentic.has_plan ? rep.agentic.score : 100.0;
    rep.score = compute_score(kpi, s, checks_ratio, forbidden, agentic_score);

    for (const auto& c : recorder.stream().calls) {
        std::string args = c.args.dump();
        if (args.size() > 160) {
            args.resize(157);
            args += "...";
        }
        rep.tool_calls.emplace_back(c.name + " [" + c.status + "]",
                                    args);
    }
    // Per-call telemetry (BENCH-11): pair each recorded call with its result
    // detail (status, error text, timeout/denied, duration) so a stored run
    // is a post-mortem, not a count.
    const auto& r_calls = recorder.stream().calls;
    const auto& r_tools = recorder.stream().tools;
    rep.total_steps = kpi.steps;
    for (size_t i = 0; i < r_calls.size(); ++i) {
        ScenarioReport::ToolDetail d;
        d.name = r_calls[i].name;
        d.args = r_calls[i].args.dump();
        d.status = r_calls[i].status;
        if (i < r_tools.size()) {
            const ToolEvent& t = r_tools[i];
            d.error = t.error;
            d.denied = t.denied;
            d.timeout = t.timeout;
            d.duration_ms = t.duration_ms;
            if (t.timeout) d.status = "timeout";
        }
        rep.tool_details.push_back(std::move(d));
    }
    // Max calls issued in a single step: group recorded calls by the loop
    // iteration they were dispatched in (recorder stamps `step`).
    {
        int max_in_step = 0;
        int cur = 0;
        int last = -1;
        std::vector<int> per_step;
        for (const auto& c : r_calls) {
            if (c.step != last) {
                if (cur > max_in_step) max_in_step = cur;
                if (cur > 0) per_step.push_back(cur);
                cur = 0;
                last = c.step;
            }
            ++cur;
        }
        if (cur > max_in_step) max_in_step = cur;
        if (cur > 0) per_step.push_back(cur);
        rep.max_calls_per_step = max_in_step;
        // calls_per_step distribution: mean + p95 (BENCH-11).
        if (!per_step.empty()) {
            double sum = 0.0;
            for (int v : per_step) sum += v;
            rep.calls_per_step_mean = sum / static_cast<double>(per_step.size());
            std::sort(per_step.begin(), per_step.end());
            const auto idx =
                static_cast<size_t>(0.95 * static_cast<double>(per_step.size() - 1));
            rep.calls_per_step_p95 = static_cast<double>(per_step[idx]);
        }
    }

    // Plan metrics (BENCH-09): adherence in dependency order, replan
    // adaptation after failures, dependency-order violations.
    {
        // Adherence = oracle steps whose match kept a strictly increasing
        // call order (the plan's dependency edges hold).
        const auto& idxs = oracle.matched_call_indexes;
        if (oracle.total_steps > 0 && !idxs.empty()) {
            size_t ordered = 1;
            for (size_t i = 1; i < idxs.size(); ++i)
                if (idxs[i] > idxs[i - 1]) ++ordered;
            rep.plan_adherence_ratio =
                static_cast<double>(ordered) / static_cast<double>(idxs.size());
        }
        // Replan: a failure followed by a different (non-failing) call.
        for (size_t i = 0; i + 1 < r_tools.size(); ++i) {
            if (!r_tools[i].ok && r_tools[i + 1].ok &&
                r_tools[i].name == r_tools[i + 1].name) {
                rep.replan_adapted = true;
                break;
            }
        }
        // Dependency violation: an ordered oracle's steps were matched out of
        // dependency order (a later step's call preceded an earlier step's),
        // or the first executed call does not satisfy the first oracle step
        // while the task requires that order (write-before-read).
        if (oracle.total_steps > 0) {
            for (size_t i = 1; i < idxs.size(); ++i)
                if (idxs[i] < idxs[i - 1]) rep.dependency_violation = true;
            if (!r_calls.empty() && !s.oracle.empty()) {
                const std::string& first = r_calls[0].name;
                const std::string& expected = s.oracle[0].tool;
                if (first != expected && oracle.matched_steps > 0 &&
                    oracle.matched_steps < oracle.total_steps)
                    rep.dependency_violation = true;
            }
        }
    }
    // Loop-control metrics: how fast a loop broke, and whether steering
    // actually let the run complete (BENCH-09).
    {
        const std::string& ft = rep.final_text;
        if (ft.find("loop detected") != std::string::npos ||
            ft.find("repeated the same tool call") != std::string::npos ||
            ft.find("repeated itself") != std::string::npos)
            rep.breakout_latency = kpi.steps;
        rep.steer_effective = kpi.recoveries > 0 && kpi.success;
    }

    if (!oracle.success) {
        std::ostringstream msg;
        msg << "oracle not matched: " << oracle.matched_steps << "/"
            << oracle.total_steps << " steps (bullseye "
            << oracle.bullseye << ")";
        rep.failures.emplace_back(msg.str());
    }
    if (!checks_ok)
        rep.failures.emplace_back("final answer failed scenario checks");
    if (kpi.hard_stop) rep.failures.emplace_back("agent hard-stopped (loop)");
    if (s.max_steps > 0 && kpi.steps > s.max_steps)
        rep.failures.emplace_back("step budget exceeded");
    if (s.max_wall_ms > 0 && kpi.wall_ms > s.max_wall_ms)
        rep.failures.emplace_back("wall-clock budget exceeded");
    if (!s.template_dir.empty()) {
        if (!kpi.compile_ok)
            rep.failures.emplace_back("artifact failed to compile");
        if (kpi.artifact_score < 1.0)
            rep.failures.emplace_back("hidden tests failed");
        if (!kpi.behavior_equivalent)
            rep.failures.emplace_back("artifact behavior differs from reference");
    }

    fs::remove_all(ws);
    return rep;
}

std::vector<ScenarioReport> run_scenarios(const std::vector<Scenario>& scenarios,
                                          const RunOptions& opts,
                                          const RunMeta& meta) {
    std::vector<ScenarioReport> out;
    for (const auto& s : scenarios) {
        if (s.hermetic_only && opts.live) continue;
        if (!opts.live && s.fake_replies.empty()) continue;
        std::string err;
        std::vector<ScenarioReport> runs;
        for (int i = 0; i < std::max(1, opts.repeat); ++i) {
            ScenarioReport rep = run_one_scenario(s, opts, meta, err);
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
