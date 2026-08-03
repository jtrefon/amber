
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

} // namespace

ScenarioReport run_one_scenario(const Scenario& s, const RunOptions& opts,
                                const RunMeta& meta, std::string& err) {
    (void)meta;
    ScenarioReport rep{s.name, s.suite, Kpi{}, Score{}, 3, "", "", {}, false, {}};
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
    agent::Workspace::set_root(ws.string());

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

    agent::ToolRegistry registry;
    agent::JobService jobs;
    agent::register_default_tools(registry, jobs, cfg.cancel_token);

    // Enforce the scenario step budget during the run (the engine's own
    // iteration cap), not just in post-hoc scoring.
    if (s.max_steps > 0 && s.max_steps < cfg.max_tool_iterations)
        cfg.max_tool_iterations = s.max_steps;

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
        auto fake = std::make_unique<FakeClient>();
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
            fake->script.push_back(std::move(r));
        }
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
    rep.score = compute_score(kpi, s, checks_ratio, forbidden);

    for (const auto& c : recorder.stream().calls) {
        std::string args = c.args.dump();
        if (args.size() > 160) {
            args.resize(157);
            args += "...";
        }
        rep.tool_calls.emplace_back(c.name + " [" + c.status + "]",
                                    args);
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
        for (int i = 0; i < std::max(1, opts.repeat); ++i) {
            ScenarioReport rep = run_one_scenario(s, opts, meta, err);
            if (i > 0) rep.name += " (repeat " + std::to_string(i) + ")";
            out.emplace_back(std::move(rep));
        }
    }
    return out;
}

} // namespace bench
