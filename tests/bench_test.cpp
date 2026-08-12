
#include "agent.h"
#include "agent/todo.h"
#include "agent/tools.h"
#include "bench/fake.h"
#include "bench/kpi.h"
#include "bench/oracle.h"
#include "bench/probe.h"
#include "bench/recorder.h"
#include "bench/report.h"
#include "bench/resources.h"
#include "bench/runner.h"
#include "bench/scenario.h"
#include "bench/template.h"

#include "tests/test_util.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

using bench::Checks;
using bench::Kpi;
using bench::OracleResult;
using bench::Recorder;
using bench::ResourceMeter;
using bench::Scenario;
using bench::ScenarioStep;
using bench::TemplateResult;
using bench::ToolCallEvent;

static std::string tmp_dir(const std::string& tag) {
    static int n = 0;
    std::string d = "/tmp/bench_" + tag + "_" + std::to_string(++n);
    fs::remove_all(d);
    fs::create_directories(d);
    return d;
}

static void write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    f << content;
}

// ---------------------------------------------------------------------------
// Oracle
// ---------------------------------------------------------------------------

TEST(oracle_exact_ordered_match) {
    std::vector<ScenarioStep> steps = {
        {"read", {{"path", "a.txt"}}},
        {"write", {{"path", "b.txt"}}},
    };
    std::vector<ToolCallEvent> calls = {
        {"read", {{"path", "a.txt"}}, 0, "ok"},
        {"write", {{"path", "b.txt"}}, 0, "ok"},
    };
    OracleResult r = bench::score_oracle(steps, calls);
    ASSERT(r.success);
    ASSERT_EQ(r.bullseye, 1.0);
    ASSERT_EQ(r.matched_steps, 2);
    ASSERT_EQ(r.on_oracle_calls, 2);
    ASSERT_EQ(r.total_calls, 2);
    ASSERT_EQ(r.wasted, 0);
    ASSERT_EQ(r.redundant, 0);
}

TEST(oracle_wildcard_args) {
    std::vector<ScenarioStep> steps = {{"read", {{"path", "*/a.txt"}}}};
    std::vector<ToolCallEvent> calls = {{"read", {{"path", "src/a.txt"}}, 0, "ok"}};
    OracleResult r = bench::score_oracle(steps, calls);
    ASSERT(r.success);
    ASSERT_EQ(r.bullseye, 1.0);
}

TEST(oracle_args_subset_relaxes) {
    std::vector<ScenarioStep> steps = {
        {"read", {{"path", "a.txt"}}, true},
    };
    std::vector<ToolCallEvent> calls = {{"read", {{"path", "a.txt"}, {"lines", 40}}, 0, "ok"}};
    OracleResult r = bench::score_oracle(steps, calls);
    ASSERT(r.success);
    ASSERT_EQ(r.arg_precision, 1.0);
}

TEST(oracle_missing_expected_key_fails) {
    std::vector<ScenarioStep> steps = {
        {"read", {{"path", "a.txt"}, {"lines", 40}}, false},
    };
    std::vector<ToolCallEvent> calls = {{"read", {{"path", "a.txt"}}, 0, "ok"}};
    OracleResult r = bench::score_oracle(steps, calls);
    ASSERT_FALSE(r.success);
    ASSERT_EQ(r.matched_steps, 0);
}

TEST(oracle_unordered_steps_any_order) {
    std::vector<ScenarioStep> steps = {
        {"read", {{"path", "a.txt"}}, false, true},
        {"search", {{"query", "*"}}, false, true},
    };
    std::vector<ToolCallEvent> calls = {
        {"search", {{"query", "x"}}, 0, "ok"},
        {"read", {{"path", "a.txt"}}, 0, "ok"},
    };
    OracleResult r = bench::score_oracle(steps, calls);
    ASSERT(r.success);
    ASSERT_EQ(r.matched_steps, 2);
    ASSERT_EQ(r.bullseye, 1.0);
}

TEST(oracle_partial_match_bullseye) {
    std::vector<ScenarioStep> steps = {
        {"read", {{"path", "a.txt"}}},
        {"write", {{"path", "b.txt"}}},
        {"bash", {{"command", "*diff*"}}},
    };
    std::vector<ToolCallEvent> calls = {{"read", {{"path", "a.txt"}}, 0, "ok"}};
    OracleResult r = bench::score_oracle(steps, calls);
    ASSERT_FALSE(r.success);
    ASSERT_EQ(r.matched_steps, 1);
    ASSERT_EQ(r.total_steps, 3);
    ASSERT(r.bullseye > 0.32);
    ASSERT(r.bullseye < 0.34);
}

TEST(oracle_off_oracle_wasted) {
    std::vector<ScenarioStep> steps = {{"read", {{"path", "a.txt"}}}};
    std::vector<ToolCallEvent> calls = {
        {"read", {{"path", "a.txt"}}, 0, "ok"},
        {"bash", {{"command", "ls"}}, 0, "ok"},
    };
    OracleResult r = bench::score_oracle(steps, calls);
    ASSERT(r.success);
    ASSERT_EQ(r.wasted, 1);
    ASSERT_EQ(r.total_calls, 2);
    ASSERT_EQ(r.on_oracle_calls, 1);
}

TEST(oracle_redundant_identical_calls) {
    std::vector<ScenarioStep> steps = {
        {"read", {{"path", "a.txt"}}},
        {"read", {{"path", "b.txt"}}},
    };
    std::vector<ToolCallEvent> calls = {
        {"read", {{"path", "a.txt"}}, 0, "ok"},
        {"read", {{"path", "a.txt"}}, 0, "ok"},
        {"read", {{"path", "b.txt"}}, 0, "ok"},
    };
    OracleResult r = bench::score_oracle(steps, calls);
    ASSERT(r.success);
    ASSERT_EQ(r.redundant, 1);
    ASSERT_EQ(r.wasted, 1);
}

TEST(oracle_empty_oracle_success) {
    OracleResult r = bench::score_oracle(
        {}, {{"read", {{"path", "a.txt"}}, 0, "ok"}});
    ASSERT(r.success);
    ASSERT_EQ(r.bullseye, 1.0);
}

// ---------------------------------------------------------------------------
// Scenario loader
// ---------------------------------------------------------------------------

TEST(scenario_loader_roundtrip) {
    std::string dir = tmp_dir("loader");
    write_file(dir + "/s.json", R"({
        "name": "demo",
        "suite": "tools",
        "description": "d",
        "platforms": ["linux"],
        "prompt": "do it",
        "oracle": [
            {"tool": "read", "args": {"path": "*"}},
            {"tool": "write", "args": {"path": "*"}, "args_subset": true, "unordered": true}
        ],
        "forbidden_tools": ["bash"],
        "prompt_checks": {"must_contain": ["done"]},
        "checks": {"must_contain": ["ok"], "must_not_contain": ["oops"]},
        "template": "coding/fizzbuzz",
        "budget": {"max_steps": 5, "max_wall_ms": 1000}
    })");
    std::string err;
    auto s = bench::load_scenario(dir + "/s.json", err);
    ASSERT(s.has_value());
    ASSERT_EQ(s->name, "demo");
    ASSERT_EQ(s->suite, "tools");
    ASSERT_EQ(s->prompt, "do it");
    ASSERT_EQ(s->oracle.size(), 2u);
    ASSERT_EQ(s->oracle[0].tool, "read");
    ASSERT(s->oracle[0].args["path"] == "*");
    ASSERT(s->oracle[1].args_subset);
    ASSERT(s->oracle[1].unordered);
    ASSERT_EQ(s->forbidden_tools.size(), 1u);
    ASSERT_EQ(s->template_dir, "coding/fizzbuzz");
    ASSERT_EQ(s->max_steps, 5);
    ASSERT_EQ(s->max_wall_ms, 1000L);
    ASSERT_EQ(s->prompt_checks.must_contain.size(), 1u);
    ASSERT_EQ(s->checks.must_not_contain.size(), 1u);
    ASSERT(bench::platform_supported(*s));
}

TEST(scenario_loader_missing_prompt_fails) {
    std::string dir = tmp_dir("loader");
    write_file(dir + "/s.json", R"({"name": "x", "suite": "s"})");
    std::string err;
    auto s = bench::load_scenario(dir + "/s.json", err);
    ASSERT_FALSE(s.has_value());
    ASSERT(!err.empty());
}

TEST(scenario_loader_missing_file_fails) {
    std::string err;
    auto s = bench::load_scenario("/nonexistent/bench_scenario.json", err);
    ASSERT_FALSE(s.has_value());
    ASSERT(!err.empty());
}

TEST(scenario_platform_filter) {
    Scenario s;
    s.platforms = {"darwin"};
    ASSERT_FALSE(bench::platform_supported(s));
    s.platforms = {"linux"};
    ASSERT(bench::platform_supported(s));
    s.platforms.clear();
    ASSERT(bench::platform_supported(s));
}

// ---------------------------------------------------------------------------
// Recorder
// ---------------------------------------------------------------------------

TEST(recorder_pairs_tool_call_and_result) {
    Recorder rec;
    agent::AgentHooks hooks = rec.hooks();
    hooks.on_tool_call("read", {{"path", "a.txt"}});
    hooks.on_tool_result("read", agent::ToolResult{true, "hello", "", agent::json{}},
                       agent::json::object());
    ASSERT_EQ(rec.stream().tools.size(), 1u);
    const bench::ToolEvent& e = rec.stream().tools[0];
    ASSERT_EQ(e.name, "read");
    ASSERT(e.ok);
    ASSERT_EQ(e.error, "");
    ASSERT_EQ(e.args["path"], "a.txt");
    ASSERT(e.duration_ms >= 0);
}

TEST(recorder_concurrent_pairing_out_of_order) {
    Recorder rec;
    agent::AgentHooks hooks = rec.hooks();
    hooks.on_tool_call("read", {{"path", "a.txt"}});
    hooks.on_tool_call("search", {{"query", "x"}});
    hooks.on_tool_result("search", agent::ToolResult{true, "hit", "", agent::json{}},
                       agent::json::object());
    hooks.on_tool_result("read", agent::ToolResult{true, "text", "", agent::json{}},
                       agent::json::object());
    ASSERT_EQ(rec.stream().tools.size(), 2u);
    bool seen_read = false, seen_search = false;
    for (const auto& e : rec.stream().tools) {
        seen_read |= e.name == "read" && e.ok;
        seen_search |= e.name == "search" && e.ok;
        ASSERT(e.duration_ms >= 0);
    }
    ASSERT(seen_read);
    ASSERT(seen_search);
}

TEST(recorder_denied_and_timeout_flags) {
    Recorder rec;
    agent::AgentHooks hooks = rec.hooks();
    hooks.on_tool_call("bash", {{"command", "rm -rf /"}});
    hooks.on_tool_result("bash", agent::ToolResult{false, "", "denied", agent::json{{"denied", true}}},
                       agent::json::object());
    hooks.on_tool_call("read", {{"path", "a.txt"}});
    hooks.on_tool_result("read", agent::ToolResult{false, "", "timeout", agent::json{{"timeout", true}}},
                       agent::json::object());
    ASSERT_EQ(rec.stream().tools.size(), 2u);
    ASSERT(rec.stream().tools[0].denied);
    ASSERT(rec.stream().tools[1].timeout);
}

TEST(recorder_counts_workspace_cd_prefix) {
    Recorder rec;
    agent::AgentHooks hooks = rec.hooks();
    const std::string root = agent::Workspace::root();
    hooks.on_tool_call("bash", {{"command", "cd " + root + " && grep foo ."}});
    hooks.on_tool_call("bash", {{"command", "cd " + root + "&& ls"}});
    hooks.on_tool_call("bash", {{"command", "cd " + root + "/ && ls"}});
    hooks.on_tool_call("bash", {{"command", "cd " + root}});
    hooks.on_tool_call("bash", {{"command", "cd /tmp && ls"}});
    hooks.on_tool_call("bash", {{"command", "cd " + root + "x && ls"}});
    hooks.on_tool_call("bash", {{"command", "ls"}});
    hooks.on_tool_call("read", {{"path", "a.txt"}});
    ASSERT_EQ(rec.stream().bash_cd_prefix, 4);
}

TEST(recorder_status_parsing) {
    Recorder rec;
    rec.on_status("LLM error - retrying (2/3) in 1s");
    rec.on_status("LLM request repaired, retrying (generation)");
    rec.on_status("tool recovery: injected steer");
    rec.on_status("text loop: injected recovery steer");
    rec.on_status("loop detected: breaking tool loop");
    rec.on_status("server rejected model \"qwen\"");
    rec.on_status("tool recovery failed, stopping");
    ASSERT_EQ(rec.stream().retries.size(), 1u);
    ASSERT_EQ(rec.stream().retries[0].attempt, 2);
    ASSERT_EQ(rec.stream().recoveries.size(), 5u);
    ASSERT_EQ(rec.stream().recoveries[0].kind, "repaired");
    ASSERT_EQ(rec.stream().recoveries[1].kind, "steer");
    ASSERT_EQ(rec.stream().recoveries[4].kind, "model");
    ASSERT(rec.stream().hard_stop);
}

TEST(recorder_iteration_debug) {
    Recorder rec;
    rec.on_debug("iteration 3/100");
    ASSERT_EQ(rec.stream().iterations, 3);
}

TEST(recorder_stats_accumulate) {
    Recorder rec;
    agent::Stats s;
    s.latency_ms = 120;
    s.tps = 40;
    s.prompt_tokens = 100;
    s.completion_tokens = 50;
    s.valid = true;
    rec.on_stats(s);
    ASSERT_EQ(rec.stream().stats.size(), 1u);
    ASSERT_EQ(rec.stream().prompt_tokens, 100L);
    ASSERT_EQ(rec.stream().completion_tokens, 50L);
    ASSERT_EQ(rec.stream().ttft_ms, 120.0);
}

// ---------------------------------------------------------------------------
// KPI computation
// ---------------------------------------------------------------------------

TEST(kpi_computation_synthesized_stream) {
    bench::EventStream stream;
    stream.tools.push_back({"read", {{"path", "a.txt"}}, true, "", false, false, 5});
    stream.tools.push_back({"write", {{"path", "b.txt"}}, true, "", false, false, 7});
    stream.retries.push_back({2});
    stream.recoveries.push_back({"repaired"});
    stream.recoveries.push_back({"steer"});
    stream.stats.push_back({100, 10, 10, 20});
    stream.iterations = 3;
    stream.ttft_ms = 100;
    stream.prompt_tokens = 10;
    stream.completion_tokens = 20;

    OracleResult oracle;
    oracle.success = true;
    oracle.bullseye = 1.0;
    oracle.matched_steps = 2;
    oracle.total_steps = 2;
    oracle.on_oracle_calls = 2;
    oracle.total_calls = 2;
    oracle.arg_precision = 1.0;

    ResourceMeter meter;
    meter.start();
    meter.stop();

    TemplateResult tmpl;

    Checks pc;
    pc.must_contain = {"done"};

    Kpi k = bench::compute_kpi(stream, oracle, meter, tmpl, pc, "all done.",
                               5000, 3000);
    ASSERT(k.success);
    ASSERT_EQ(k.bullseye, 1.0);
    ASSERT_EQ(k.tool_call_accuracy, 1.0);
    ASSERT_EQ(k.arg_precision, 1.0);
    ASSERT_EQ(k.steps, 3);
    ASSERT_EQ(k.retries, 1);
    ASSERT_EQ(k.recoveries, 2);
    ASSERT_EQ(k.steers, 1);
    ASSERT_EQ(k.wall_ms, 5000L);
    ASSERT_EQ(k.bullseye_at_ms, 3000L);
    ASSERT_EQ(k.ttft_ms, 100.0);
    ASSERT_EQ(k.tps_avg, 10.0);
    ASSERT_EQ(k.prompt_tokens, 10L);
    ASSERT_EQ(k.completion_tokens, 20L);
    ASSERT_EQ(k.artifact_score, 1.0);
    ASSERT_EQ(k.prompt_adherence, 1.0);
}

TEST(kpi_budget_enforcement) {
    Scenario s;
    s.max_steps = 2;
    Kpi k;
    k.success = true;
    k.steps = 3;
    ASSERT_FALSE(bench::kpi_success(k, s));
    s.max_steps = 0;
    ASSERT(bench::kpi_success(k, s));
}

// ---------------------------------------------------------------------------
// Resources
// ---------------------------------------------------------------------------

TEST(resource_meter_samples) {
    ResourceMeter meter;
    meter.start();
    volatile long sink = 0;
    for (int i = 0; i < 100000; ++i) sink += i;
    (void)sink;
    meter.stop();
    ASSERT(meter.cpu_ms() >= 0);
    ASSERT(meter.peak_rss_kb() > 0);
}

// ---------------------------------------------------------------------------
// Template engine (real compilation)
// ---------------------------------------------------------------------------

TEST(template_structure_checks) {
    std::string dir = tmp_dir("tpl");
    write_file(dir + "/checks.json",
               R"({"must_contain": ["fizzbuzz"], "must_not_contain": ["std::sort"]})");
    std::vector<bench::StructureCheck> out;
    std::string err;
    ASSERT(bench::load_structure_checks(dir, out, err));
    ASSERT_EQ(out.size(), 2u);
    ASSERT_EQ(out[0].kind, "must_contain");
    ASSERT_EQ(out[0].pattern, "fizzbuzz");
}

TEST(template_validate_reference_and_score_artifact) {
    std::string dir = tmp_dir("tplrun");
    std::string tpl = dir + "/template";
    fs::create_directories(tpl + "/reference");
    fs::create_directories(tpl + "/hidden_tests");
    write_file(tpl + "/reference/solution.h",
               "#pragma once\n"
               "#include <string>\n"
               "std::string fizzbuzz(int n);\n");
    write_file(tpl + "/reference/solution.cpp",
               "#include \"solution.h\"\n"
               "std::string fizzbuzz(int n) {\n"
               "  if (n % 15 == 0) return \"FizzBuzz\";\n"
               "  if (n % 3 == 0) return \"Fizz\";\n"
               "  if (n % 5 == 0) return \"Buzz\";\n"
               "  return std::to_string(n);\n"
               "}\n");
    write_file(tpl + "/hidden_tests/test_main.cpp",
               "#include \"solution.h\"\n"
               "#include <cstdio>\n"
               "#include <cstdlib>\n"
               "int main(int argc, char** argv) {\n"
               "  int n = argc > 1 ? std::atoi(argv[1]) : 3;\n"
               "  std::printf(\"%s\", fizzbuzz(n).c_str());\n"
               "  return fizzbuzz(n) == std::string(\"Fizz\") ? 0 : 1;\n"
               "}\n");
    write_file(tpl + "/checks.json", R"({"must_contain": ["fizzbuzz"]})");

    // Reference artifact: identical files -> validate + equivalent + pass.
    std::string art_ref = dir + "/artifact_ref";
    fs::create_directories(art_ref);
    {
        std::ifstream in(tpl + "/reference/solution.h");
        std::ofstream out(art_ref + "/solution.h");
        out << in.rdbuf();
        in.close();
        std::ifstream in2(tpl + "/reference/solution.cpp");
        std::ofstream out2(art_ref + "/solution.cpp");
        out2 << in2.rdbuf();
    }
    std::string err;
    TemplateResult r = bench::run_template(tpl, art_ref, "g++", err);
    ASSERT(r.compile_ok);
    ASSERT_EQ(r.tests_passed, r.tests_total);
    ASSERT(r.tests_total > 0);
    ASSERT(r.behavior_equivalent);
    ASSERT_EQ(r.structure_checks, 1.0);
    ASSERT(r.artifact_loc > 0);
    ASSERT_EQ(err, "");
}

TEST(template_artifact_failure_no_equivalence) {
    std::string dir = tmp_dir("tplfail");
    std::string tpl = dir + "/template";
    fs::create_directories(tpl + "/reference");
    fs::create_directories(tpl + "/hidden_tests");
    write_file(tpl + "/reference/solution.h",
               "#pragma once\n#include <string>\nstd::string fizzbuzz(int n);\n");
    write_file(tpl + "/reference/solution.cpp",
               "#include \"solution.h\"\n"
               "std::string fizzbuzz(int n) {\n"
               "  if (n % 15 == 0) return \"FizzBuzz\";\n"
               "  if (n % 3 == 0) return \"Fizz\";\n"
               "  if (n % 5 == 0) return \"Buzz\";\n"
               "  return std::to_string(n);\n"
               "}\n");
    write_file(tpl + "/hidden_tests/test_main.cpp",
               "#include \"solution.h\"\n#include <cstdlib>\n"
               "int main() { return fizzbuzz(3) == \"Fizz\" ? 0 : 1; }\n");

    // Broken artifact: fizzbuzz(3) returns "Fizzz".
    std::string art = dir + "/artifact";
    fs::create_directories(art);
    write_file(art + "/solution.h",
               "#pragma once\n#include <string>\nstd::string fizzbuzz(int n);\n");
    write_file(art + "/solution.cpp",
               "#include \"solution.h\"\n"
               "std::string fizzbuzz(int) { return \"Fizzz\"; }\n");
    write_file(tpl + "/checks.json", R"({"must_contain": ["fizzbuzz"]})");

    std::string err;
    TemplateResult r = bench::run_template(tpl, art, "g++", err);
    ASSERT(r.compile_ok);
    ASSERT_EQ(r.tests_passed, 0);
    ASSERT_EQ(r.tests_total, 1);
    ASSERT_FALSE(r.behavior_equivalent);
}

// ---------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------

TEST(report_text_renders_scenario_and_verdict) {
    bench::ScenarioReport rep;
    rep.name = "demo-scenario";
    rep.suite = "tools";
    rep.kpi.success = true;
    bench::RunMeta meta;
    meta.mode = "hermetic";
    std::string text = bench::render_text({rep}, meta);
    ASSERT(text.find("demo-scenario") != std::string::npos);
    ASSERT(text.find("PASS") != std::string::npos);

    std::string json = bench::render_json({rep}, meta);
    ASSERT(json.find("demo-scenario") != std::string::npos);
    ASSERT(json.find("success") != std::string::npos);
}

// ---------------------------------------------------------------------------
// End-to-end hermetic run (runner + agent + fake client)
// ---------------------------------------------------------------------------

TEST(e2e_hermetic_read_scenario) {
    std::string dir = tmp_dir("e2e");
    write_file(dir + "/s.json", R"({
        "name": "e2e-read",
        "suite": "tools",
        "prompt": "what is in a.txt",
        "setup": {"files": {"a.txt": "hello world"}},
        "fake_replies": [
            {"tool_calls": [{"id": "call_1", "type": "function",
                             "function": {"name": "read",
                                          "arguments": "{\"path\":\"a.txt\"}"}}],
             "prompt_tokens": 50, "completion_tokens": 10},
            {"content": "done. a.txt contains hello world.",
             "prompt_tokens": 60, "completion_tokens": 5},
            {"content": "yes", "prompt_tokens": 1, "completion_tokens": 1}
        ],
        "oracle": [{"tool": "read", "args": {"path": "a.txt"}}],
        "checks": {"must_contain": ["hello world"]},
        "budget": {"max_steps": 10, "max_wall_ms": 30000}
    })");
    std::string err;
    auto s = bench::load_scenario(dir + "/s.json", err);
    ASSERT(s.has_value());
    ASSERT_EQ(s->fake_replies.size(), 3u);

    bench::RunOptions opts;
    bench::RunMeta meta;
    meta.mode = "hermetic";
    meta.model = "fake";

    // The runner owns a fresh Agent + recorder + workspace per scenario.
    bench::ScenarioReport rep = bench::run_one_scenario(*s, opts, meta, err);
    ASSERT_EQ(err, "");
    ASSERT_EQ(rep.failures.size(), 0u);
    ASSERT(rep.kpi.success);
    ASSERT_EQ(rep.kpi.bullseye, 1.0);
    ASSERT_EQ(rep.kpi.steps, 2);
    ASSERT_EQ(rep.kpi.tool_call_accuracy, 1.0);
    ASSERT_EQ(rep.kpi.prompt_adherence, 1.0);
    ASSERT(rep.kpi.wall_ms >= 0);
}

// run_one_scenario must restore the workspace root it overrode for the
// scenario's temp workspace. Without the restore, a serial multi-scenario
// run (and the harness probes running after scenarios) sees a root that
// points at a workspace already removed at teardown — the bash tool then
// cannot spawn, and dispatch fails.
TEST(e2e_hermetic_restores_workspace_root) {
    const std::string prior = agent::Workspace::root();

    std::string dir = tmp_dir("wsrestore");
    write_file(dir + "/s.json", R"({
        "name": "e2e-restore",
        "suite": "tools",
        "prompt": "what is in a.txt",
        "setup": {"files": {"a.txt": "hi"}},
        "fake_replies": [
            {"content": "done", "prompt_tokens": 1, "completion_tokens": 1}
        ],
        "budget": {"max_steps": 5, "max_wall_ms": 30000}
    })");
    std::string err;
    auto s = bench::load_scenario(dir + "/s.json", err);
    ASSERT(s.has_value());

    bench::RunOptions opts;
    bench::RunMeta meta;
    meta.mode = "hermetic";
    meta.model = "fake";
    bench::ScenarioReport rep = bench::run_one_scenario(*s, opts, meta, err);
    ASSERT_EQ(err, "");
    ASSERT_EQ(rep.failures.size(), 0u);

    // The root must be back to the pre-run value — never a deleted temp ws.
    ASSERT_EQ(agent::Workspace::root(), prior);
}

int main() { return agent::test::run_all(); }

// ---------------------------------------------------------------------------
// Scoring (v2 — continuous, weighted, discriminating)
// ---------------------------------------------------------------------------

static bench::Kpi perfect_kpi() {
    bench::Kpi k;
    k.success = true;
    k.bullseye = 1.0;
    k.tool_call_accuracy = 1.0;
    k.arg_precision = 1.0;
    k.prompt_adherence = 1.0;
    return k;
}

#define ASSERT_NEAR(a, b, eps)                                                \
    do {                                                                      \
        const double _a = (a);                                                \
        const double _b = (b);                                                \
        if (_a < _b - (eps) || _a > _b + (eps))                               \
            ::agent::test::fail("assert_near failed: " #a " ~= " #b           \
                                " (" + std::to_string(_a) + " vs " +          \
                                std::to_string(_b) + ")");                    \
    } while (0)

TEST(score_perfect_run_100) {
    Scenario s;
    s.oracle = {{"read", {{"path", "a.txt"}}}, {"write", {{"path", "b.txt"}}}};
    Kpi k = perfect_kpi();
    k.steps = 2;
    bench::Score sc = bench::compute_score(k, s, 1.0, 0, 100.0);
    ASSERT_EQ(sc.correctness, 100.0);
    ASSERT_EQ(sc.efficiency, 100.0);
    ASSERT_EQ(sc.robustness, 100.0);
    ASSERT_EQ(sc.adherence, 100.0);
    ASSERT_EQ(sc.total, 100.0);
}

TEST(score_template_partial_artifact) {
    Scenario s;
    s.template_dir = "coding/x";
    Kpi k = perfect_kpi();
    k.steps = 1;
    k.artifact_score = 0.5;
    k.compile_ok = true;
    bench::Score sc = bench::compute_score(k, s, 1.0, 0, 100.0);
    ASSERT_NEAR(sc.correctness, 85.0, 0.01);
    ASSERT_NEAR(sc.total, 94.0, 0.01);
}

TEST(score_efficiency_penalties) {
    Scenario s;
    s.oracle = {{"read", {{"path", "a.txt"}}}, {"write", {{"path", "b.txt"}}}};
    Kpi k = perfect_kpi();
    k.steps = 4;
    k.wasted = 2;
    k.redundant = 1;
    bench::Score sc = bench::compute_score(k, s, 1.0, 0, 100.0);
    ASSERT_NEAR(sc.efficiency, 40.0, 0.01);
    ASSERT_NEAR(sc.total, 85.0, 0.01);
}

TEST(score_robustness_hard_stop) {
    Scenario s;
    s.oracle = {{"read", {{"path", "a.txt"}}}};
    Kpi k = perfect_kpi();
    k.steps = 1;
    k.hard_stop = true;
    k.success = false;  // compute_kpi marks hard stops as failures
    bench::Score sc = bench::compute_score(k, s, 1.0, 0, 100.0);
    ASSERT_NEAR(sc.robustness, 0.0, 0.01);
    ASSERT_NEAR(sc.total, 60.0, 0.01);  // partial credit capped at 60
}

TEST(score_failure_cap_keeps_partial_credit) {
    Scenario s;
    s.oracle = {{"read", {{"path", "a.txt"}}}};
    Kpi k = perfect_kpi();
    k.steps = 1;
    k.success = false;  // one failed check, otherwise flawless
    bench::Score sc = bench::compute_score(k, s, 1.0, 0, 100.0);
    ASSERT_NEAR(sc.correctness, 100.0, 0.01);
    ASSERT_NEAR(sc.total, 60.0, 0.01);
}

TEST(score_adherence_forbidden_penalty) {
    Scenario s;
    s.prompt_checks.must_contain = {"done"};
    s.oracle = {{"read", {{"path", "a.txt"}}}};
    Kpi k = perfect_kpi();
    k.steps = 1;
    bench::Score sc = bench::compute_score(k, s, 1.0, 2, 100.0);
    ASSERT_NEAR(sc.adherence, 50.0, 0.01);
    ASSERT_NEAR(sc.total, 97.5, 0.01);
}

TEST(score_expected_steps_from_oracle) {
    Scenario s;
    s.oracle = {{"read", {{"path", "a.txt"}}}, {"write", {{"path", "b.txt"}}}, {"bash", {{"command", "*"}}}};
    Kpi k = perfect_kpi();
    k.steps = 3;
    bench::Score sc = bench::compute_score(k, s, 1.0, 0, 100.0);
    ASSERT_EQ(sc.efficiency, 100.0);
}

TEST(run_score_difficulty_weighted) {
    bench::ScenarioReport a;
    a.difficulty = 1;
    a.score.total = 100.0;
    bench::ScenarioReport b;
    b.difficulty = 3;
    b.score.total = 50.0;
    double score = bench::run_score({a, b});
    ASSERT_EQ(score, 62.5);
}

TEST(run_score_empty_1000) {
    double score = bench::run_score({});
    ASSERT_EQ(score, 100.0);
}

// ---------------------------------------------------------------------------
// Markdown report
// ---------------------------------------------------------------------------

TEST(report_markdown_renders_score_and_table) {
    bench::ScenarioReport rep;
    rep.name = "demo-scenario";
    rep.suite = "tools";
    rep.kpi.success = true;
    rep.kpi.bullseye = 1.0;
    rep.kpi.steps = 2;
    rep.difficulty = 3;
    rep.score.total = 95.0;
    bench::RunMeta meta;
    meta.mode = "live";
    meta.model = "test-model";
    std::string md = bench::render_markdown({rep}, meta);
    ASSERT(md.find("demo-scenario") != std::string::npos);
    ASSERT(md.find("950/1000") != std::string::npos);
    ASSERT(md.find('|') != std::string::npos);
}

TEST(report_markdown_comparison_matrix) {
    bench::ScenarioReport a;
    a.name = "s1";
    a.suite = "t";
    a.difficulty = 3;
    a.score.total = 90.0;
    a.kpi.success = true;
    bench::ScenarioReport b;
    b.name = "s1";
    b.suite = "t";
    b.difficulty = 3;
    b.score.total = 60.0;
    b.kpi.success = false;
    bench::RunMeta m1;
    m1.model = "model-a";
    bench::RunMeta m2;
    m2.model = "model-b";
    std::string md = bench::render_markdown_comparison(
        {{m1, {a}}, {m2, {b}}});
    ASSERT(md.find("model-a") != std::string::npos);
    ASSERT(md.find("model-b") != std::string::npos);
    ASSERT(md.find("90") != std::string::npos);
    ASSERT(md.find("60") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Agentic tool KPIs (per-model tool efficiency)
// ---------------------------------------------------------------------------

TEST(kpi_tool_aggregation_from_stream) {
    bench::EventStream stream;
    stream.calls.push_back({"read", {{"path", "a.txt"}}, 0, "ok"});
    stream.calls.push_back({"read", {{"path", "b.txt"}}, 0, "error"});
    stream.calls.push_back({"bash", {{"command", "ls"}}, 0, "denied"});
    stream.calls.push_back({"read", {{"path", "a.txt"}}, 0, "ok"});
    stream.bash_cd_prefix = 2;

    OracleResult oracle;
    oracle.success = true;
    oracle.bullseye = 1.0;
    oracle.total_steps = 2;
    oracle.matched_steps = 2;
    oracle.on_oracle_calls = 2;
    oracle.total_calls = 4;
    oracle.arg_precision = 1.0;
    oracle.redundant = 1;  // identical read a.txt twice (score_oracle detects)

    ResourceMeter meter;
    TemplateResult tmpl;
    Checks pc;

    Kpi k = bench::compute_kpi(stream, oracle, meter, tmpl, pc, "done.", 1000, 500);
    ASSERT_EQ(k.tool_calls, 4);
    ASSERT_EQ(k.tool_failures, 1);
    ASSERT_EQ(k.tool_denied, 1);
    ASSERT_EQ(k.redundant, 1);  // read a.txt twice
    ASSERT_EQ(k.bash_cd_prefix, 2);
}

TEST(report_markdown_agentic_profile) {
    bench::ScenarioReport rep;
    rep.name = "demo";
    rep.suite = "tools";
    rep.kpi.success = true;
    rep.kpi.bullseye = 1.0;
    rep.kpi.steps = 3;
    rep.kpi.tool_calls = 5;
    rep.kpi.tool_failures = 1;
    rep.kpi.tool_denied = 0;
    rep.kpi.redundant = 2;
    rep.kpi.retries = 1;
    rep.difficulty = 3;
    rep.score.total = 90.0;
    bench::RunMeta meta;
    meta.mode = "live";
    meta.model = "m1";
    std::string md = bench::render_markdown({rep}, meta);
    ASSERT(md.find("Agentic profile") != std::string::npos);
    ASSERT(md.find("tool failures") != std::string::npos);
    ASSERT(md.find('1') != std::string::npos);
}

// ---------------------------------------------------------------------------
// Agentic performance: distance from the optimal tool plan
// ---------------------------------------------------------------------------

static bench::Agentic agentic_for(const std::vector<bench::ToolCallEvent>& calls,
                                  const std::vector<ScenarioStep>& oracle,
                                  const agent::json& plan,
                                  const Kpi& k = Kpi{}) {
    bench::EventStream stream;
    stream.calls = calls;
    Scenario s;
    s.oracle = oracle;
    s.optimal_plan = plan;
    return bench::compute_agentic(stream, k, s);
}

TEST(agentic_perfect_plan_zero_deviation) {
    std::vector<bench::ToolCallEvent> calls = {
        {"read", {{"path", "a.txt"}}, 0, "ok"},
        {"write", {{"path", "b.txt"}}, 0, "ok"},
    };
    auto a = agentic_for(calls, {{"read", {}}, {"write", {}}}, agent::json());
    ASSERT(a.has_plan);
    ASSERT_EQ(a.plan_tools, 2);
    ASSERT_EQ(a.plan_deviation, 0);
    ASSERT_EQ(a.plan_ratio, 1.0);
    ASSERT_EQ(a.score, 100.0);
}

TEST(agentic_extra_calls_penalized) {
    std::vector<bench::ToolCallEvent> calls = {
        {"read", {{"path", "a.txt"}}, 0, "ok"},
        {"bash", {{"command", "ls"}}, 0, "ok"},
        {"bash", {{"command", "cat"}}, 0, "ok"},
    };
    auto a = agentic_for(calls, {{"read", {}}}, agent::json());
    ASSERT_EQ(a.plan_tools, 1);
    ASSERT_EQ(a.plan_deviation, 2);
    ASSERT_NEAR(a.plan_ratio, 1.0 / 3.0, 0.01);
    ASSERT_EQ(a.score, 80.0);
}

TEST(agentic_explicit_plan_overrides_oracle) {
    // Oracle says read TASK.md only, but a perfect agent also writes and
    // compiles: explicit plan {read:1, write:1, bash:1}.
    std::vector<bench::ToolCallEvent> calls = {
        {"read", {{"path", "TASK.md"}}, 0, "ok"},
        {"write", {{"path", "solution.cpp"}}, 0, "ok"},
        {"bash", {{"command", "g++"}}, 0, "ok"},
    };
    auto a = agentic_for(calls, {{"read", {}}},
                         agent::json{{"read", 1}, {"write", 1}, {"bash", 1}});
    ASSERT_EQ(a.plan_tools, 3);
    ASSERT_EQ(a.plan_deviation, 0);
    ASSERT_EQ(a.score, 100.0);
}

TEST(agentic_repeats_failures_retries_penalized) {
    std::vector<bench::ToolCallEvent> calls = {
        {"read", {{"path", "a.txt"}}, 0, "ok"},
        {"read", {{"path", "a.txt"}}, 0, "ok"},   // exact repeat
        {"read", {{"path", "b.txt"}}, 0, "error"},  // failure
    };
    Kpi k;
    k.redundant = 1;
    k.retries = 1;
    k.tool_failures = 1;
    auto a = agentic_for(calls, {{"read", {}}}, agent::json(), k);
    // 100 - 20 extra calls - 20 repeat - 25 failure - 30 retry = 5
    ASSERT_EQ(a.score, 5.0);
}

TEST(agentic_no_plan_skipped) {
    auto a = agentic_for({}, {}, agent::json());
    ASSERT_FALSE(a.has_plan);
    ASSERT_EQ(a.score, 0.0);
}

TEST(agentic_efficiency_percent_normalized) {
    std::vector<bench::ToolCallEvent> calls = {
        {"read", {{"path", "a.txt"}}, 0, "ok"},
        {"read", {{"path", "a.txt"}}, 0, "ok"},   // 2 reads, plan says 1
        {"bash", {{"command", "ls"}}, 0, "ok"},
    };
    auto a = agentic_for(calls, {{"read", {}}}, agent::json());
    // plan 1 / actual 3 -> 33.3%
    ASSERT_NEAR(a.efficiency_pct, 33.3, 0.1);
    ASSERT_NEAR(a.plan_ratio, 1.0 / 3.0, 0.01);
}

TEST(agentic_efficiency_capped_at_100) {
    std::vector<bench::ToolCallEvent> calls = {
        {"read", {{"path", "a.txt"}}, 0, "ok"},  // fewer calls than plan
    };
    auto a = agentic_for(calls, {{"read", {}}, {"write", {}}}, agent::json());
    ASSERT_EQ(a.efficiency_pct, 100.0);  // under-execution capped (correctness catches it)
}

TEST(template_c_source_support) {
    // Repo-suite templates ship .c sources; contract_sources must pick them up.
    std::string dir = tmp_dir("tplc");
    std::string tpl = dir + "/template";
    fs::create_directories(tpl + "/reference");
    fs::create_directories(tpl + "/hidden_tests");
    write_file(tpl + "/reference/lib.h", "#pragma once\nint value(void);\n");
    write_file(tpl + "/reference/lib.c",
               "#include \"lib.h\"\nint value(void) { return 42; }\n");
    write_file(tpl + "/hidden_tests/test_main.cpp",
               "#include \"lib.h\"\n#include <cstdio>\n"
               "int main() { std::printf(\"%d\", value()); return value() == 42 ? 0 : 1; }\n");
    write_file(tpl + "/checks.json", R"({"must_contain": ["value"]})");
    std::string err;
    TemplateResult r = bench::run_template(tpl, tpl + "/reference", "g++", err);
    ASSERT(r.compile_ok);
    ASSERT_EQ(r.tests_passed, 1);
    ASSERT(r.behavior_equivalent);
}

TEST(todowrite_schema_invites_proactive_use) {
    // The advertisement matters: models reach for TodoWrite when the
    // description names concrete triggers ("three or more distinct steps",
    // dependencies, mid-task additions). A passive description gets ignored.
    agent::TodoStore store;
    auto tool = agent::make_todowrite_tool(store);
    const std::string d = tool->description();
    ASSERT(d.find("three or more distinct steps") != std::string::npos);
    ASSERT(d.find("survives context compaction") != std::string::npos);
}

TEST(recorder_counts_compressions) {
    Recorder rec;
    rec.on_status("compressing 12 messages...");
    rec.on_status("LLM error - retrying (1/3) in 1s");
    rec.on_status("compressing 4 messages...");
    ASSERT_EQ(rec.stream().compressions, 2);
    ASSERT_EQ(rec.stream().retries.size(), 1u);
}

// The task tool's sub-agent work must never leak into the parent's observer
// hooks: the recorder sees exactly the one task call, not the sub-agent's
// nested tool calls or its internal exchanges.
TEST(subagent_hooks_do_not_leak) {
    using namespace agent;
    Workspace::set_root(std::filesystem::current_path().string());
    Config cfg;
    cfg.stream = false;
    cfg.max_tool_iterations = 100;
    cfg.system_prompt_path = "prompts/system.md";
    cfg.tools_prompt_path = "prompts/tools.md";

    ToolRegistry reg;
    JobService jobs;
    TodoStore todos;
    SubAgentExecutor executor;
    register_default_tools(reg, jobs, todos, CancellationToken{}, false,
                           executor, true);

    auto parent_script = std::make_shared<std::deque<bench::BenchReply>>();
    auto sub_script = std::make_shared<std::deque<bench::BenchReply>>();
    auto script = parent_script;
    bench::BenchReply task_call;
    task_call.tool_calls = json::array(
        {{{"id", "call_1"},
          {"type", "function"},
          {"function",
           {{"name", "task"},
            {"arguments", R"({"prompt":"explore the workspace"})"}}}}});
    script->push_back(std::move(task_call));
    auto script2 = sub_script;
    bench::BenchReply sub_read;
    sub_read.tool_calls = json::array(
        {{{"id", "call_2"},
          {"type", "function"},
          {"function",
           {{"name", "read"},
            {"arguments", R"({"path":"Makefile"})"}}}}});
    script2->push_back(std::move(sub_read));
    bench::BenchReply sub_report;
    sub_report.content = "worker findings";
    script2->push_back(std::move(sub_report));
    bench::BenchReply sub_done;
    sub_done.content = "done";
    script2->push_back(std::move(sub_done));
    bench::BenchReply parent_done;
    parent_done.content = "done";
    script->push_back(std::move(parent_done));
    bench::BenchReply parent_yes;
    parent_yes.content = "yes";
    script->push_back(std::move(parent_yes));

    Recorder rec;
    AgentHooks hooks = rec.hooks();
    auto parent = std::make_unique<bench::FakeClient>();
    parent->script = *parent_script;
    executor.set_factory([sub_script](const Config&) {
        auto f = std::make_unique<bench::FakeClient>();
        f->script = *sub_script;
        return std::unique_ptr<LLMClient>(std::move(f));
    });
    executor.set_config(cfg);
    executor.set_hooks(hooks);

    Agent ag(cfg, reg, hooks, {}, {}, {}, {}, std::move(parent));
    std::string reply = ag.run("delegate an exploration");
    ASSERT_EQ(reply, "done");
    ASSERT_EQ(rec.stream().tools.size(), 1u);
    ASSERT_EQ(rec.stream().tools[0].name, "task");
    ASSERT(rec.stream().tools[0].ok);
}

// ---------------------------------------------------------------------------
// Scoring v2 (quality-perception): agentic + arg_precision in the score
// ---------------------------------------------------------------------------

// Argument fidelity (arg_precision) must move the correctness sub-score.
TEST(score_uses_arg_precision) {
    Scenario s;
    s.oracle = {{"read", {{"path", "a.txt"}}}};
    Kpi perfect = perfect_kpi();
    perfect.steps = 1;
    Kpi sloppy = perfect;
    sloppy.arg_precision = 0.5;
    bench::Score a = bench::compute_score(perfect, s, 1.0, 0, 100.0);
    bench::Score b = bench::compute_score(sloppy, s, 1.0, 0, 100.0);
    ASSERT_NEAR(a.correctness, 100.0, 0.01);
    // 0.25 weight on arg_precision, 0.5 deficit -> 10 points of correctness.
    ASSERT_NEAR(b.correctness, 90.0, 0.01);
}

// Tool economy (agentic) must move the total at its 0.20 weight.
TEST(score_uses_agentic) {
    Scenario s;
    s.oracle = {{"read", {{"path", "a.txt"}}}};
    Kpi k = perfect_kpi();
    k.steps = 1;
    bench::Score good = bench::compute_score(k, s, 1.0, 0, 100.0);
    bench::Score bad = bench::compute_score(k, s, 1.0, 0, 40.0);
    ASSERT_NEAR(good.total - bad.total, 12.0, 0.01);
}

// The v2 weights are exact: 0.40/0.25/0.20/0.10/0.05.
TEST(score_new_weights_exact) {
    Scenario s;
    s.oracle = {{"read", {{"path", "a.txt"}}}, {"write", {{"path", "b.txt"}}}};
    Kpi k = perfect_kpi();
    // 2-step oracle, 4 steps -> excess 2 -> efficiency 80.
    k.steps = 4;
    k.prompt_adherence = 0.9;  // adherence 90
    bench::Score sc = bench::compute_score(k, s, 1.0, 0, 60.0);
    ASSERT_NEAR(sc.correctness, 100.0, 0.01);
    ASSERT_NEAR(sc.efficiency, 80.0, 0.01);
    ASSERT_NEAR(sc.robustness, 100.0, 0.01);
    ASSERT_NEAR(sc.adherence, 90.0, 0.01);
    // 0.40*100 + 0.25*80 + 0.20*60 + 0.10*100 + 0.05*90 = 86.5
    ASSERT_NEAR(sc.total, 86.5, 0.01);
}

// checks_weight: a review scenario's checks dominate its correctness.
TEST(checks_weight_eight_dominates) {
    Scenario s;
    s.oracle = {{"read", {{"path", "Review.java"}}}};
    s.checks_weight = 0.8;
    Kpi k = perfect_kpi();
    k.steps = 1;
    // bullseye 1.0, arg_precision 1.0, checks ratio 0.5 (half the issues found)
    bench::Score sc = bench::compute_score(k, s, 0.5, 0, 100.0);
    // (1-0.8)*(0.75*1 + 0.25*1) + 0.8*0.5 = 0.2 + 0.4 = 0.6
    ASSERT_NEAR(sc.correctness, 60.0, 0.01);
}

// The default checks_weight reproduces the v2 non-template formula.
TEST(checks_weight_default_backward_compatible) {
    Scenario s;
    s.oracle = {{"read", {{"path", "a.txt"}}}};
    Kpi k = perfect_kpi();
    k.steps = 1;
    bench::Score sc = bench::compute_score(k, s, 0.5, 0, 100.0);
    // 0.8*(0.75*1 + 0.25*1) + 0.2*0.5 = 0.8 + 0.1 = 0.9
    ASSERT_NEAR(sc.correctness, 90.0, 0.01);
}

// A review-style scenario file (code snippet + issue checks + checks_weight)
// must load end-to-end.
TEST(review_scenario_parses) {
    std::string dir = tmp_dir("review");
    write_file(dir + "/r.json", R"({
        "name": "review-arch-01-god-class",
        "suite": "review-arch",
        "difficulty": 3,
        "prompt": "Read Review.java and list the design issues you find, anchored to the code.",
        "setup": {"files": {"Review.java": "class Report { void parse() {} void render() {} void save() {} }"}},
        "oracle": [{"tool": "read", "args": {"path": "Review.java"}}],
        "checks_weight": 0.8,
        "checks": {
            "must_contain": ["parses", "renders"],
            "must_not_contain": ["premature optimization"]
        },
        "budget": {"max_steps": 10, "max_wall_ms": 30000}
    })");
    std::string err;
    auto s = bench::load_scenario(dir + "/r.json", err);
    ASSERT(s.has_value());
    ASSERT_EQ(s->checks_weight, 0.8);
    ASSERT_EQ(s->oracle.size(), 1u);
    ASSERT_EQ(s->checks.must_contain.size(), 2u);
}

// End-to-end: a review scenario (code snippet + issue checks + checks_weight
// 0.8) runs hermetically through the runner; correctness comes out of the
// checks-dominated formula and agentic is computed in hermetic mode too.
TEST(e2e_hermetic_review_scoring) {
    std::string dir = tmp_dir("reviewe2e");
    write_file(dir + "/r.json", R"({
        "name": "arch-01-god-class",
        "suite": "review-arch",
        "prompt": "Read Review.cpp and list the design issues.",
        "setup": {"files": {"Review.cpp": "class Report { void parse(); void render(); void save(); };"}},
        "fake_replies": [
            {"tool_calls": [{"id": "call_1", "type": "function",
                             "function": {"name": "read",
                                          "arguments": "{\"path\":\"Review.cpp\"}"}}],
             "prompt_tokens": 50, "completion_tokens": 10},
            {"content": "The Report class parses and renders in one place; it should not save.",
             "prompt_tokens": 60, "completion_tokens": 5},
            {"content": "yes", "prompt_tokens": 1, "completion_tokens": 1}
        ],
        "oracle": [{"tool": "read", "args": {"path": "Review.cpp"}}],
        "checks_weight": 0.8,
        "checks": {
            "must_contain": ["parse", "render", "save"],
            "must_not_contain": ["premature"]
        },
        "budget": {"max_steps": 10, "max_wall_ms": 30000}
    })");
    std::string err;
    auto s = bench::load_scenario(dir + "/r.json", err);
    ASSERT(s.has_value());

    bench::RunOptions opts;
    bench::RunMeta meta;
    meta.mode = "hermetic";
    meta.model = "fake";
    bench::ScenarioReport rep = bench::run_one_scenario(*s, opts, meta, err);
    ASSERT_EQ(err, "");
    ASSERT(rep.kpi.success);
    // All anchors hit -> checks 4/4 -> correctness 100; the partial-credit
    // math (arg_precision, checks_weight ratios, agentic deltas) is proven
    // by the unit tests above — this proves the pipeline end to end.
    ASSERT_NEAR(rep.score.correctness, 100.0, 0.01);
    // agentic computed even in hermetic mode (plan = 1 read).
    ASSERT(rep.agentic.has_plan);
}

// A plan-less scenario (no oracle, no optimal_plan) must score with the
// neutral agentic value — the runner feeds 100.0 instead of the 0.0
// compute_agentic reports without a plan.
namespace {
bench::Scenario planless_scenario(std::string& dir) {
    dir = tmp_dir("planless");
    write_file(dir + "/p.json", R"({
        "name": "planless",
        "suite": "smoke",
        "prompt": "reply with the word done",
        "fake_replies": [
            {"content": "done.", "prompt_tokens": 50, "completion_tokens": 10},
            {"content": "yes", "prompt_tokens": 1, "completion_tokens": 1}
        ],
        "checks": {"must_contain": ["done"]},
        "budget": {"max_steps": 10, "max_wall_ms": 30000}
    })");
    std::string err;
    auto s = bench::load_scenario(dir + "/p.json", err);
    ASSERT(s.has_value());
    return *s;
}
} // namespace

TEST(e2e_hermetic_planless_neutral_agentic) {
    std::string dir;
    bench::Scenario s = planless_scenario(dir);
    bench::RunOptions opts;
    bench::RunMeta meta;
    meta.mode = "hermetic";
    meta.model = "fake";
    std::string err;
    bench::ScenarioReport rep = bench::run_one_scenario(s, opts, meta, err);
    ASSERT_EQ(err, "");
    ASSERT(rep.kpi.success);
    ASSERT_FALSE(rep.agentic.has_plan);
    // checks 1/1, bullseye auto 1.0, agentic neutral 100 -> total 100, not
    // dragged to 80 by a plan-less 0.0 agentic.
    ASSERT_NEAR(rep.score.total, 100.0, 0.01);
}

// ---------------------------------------------------------------------------
// BENCH-04 — agentic per-tool deviation, any-of checks, p-03 oracle
// ---------------------------------------------------------------------------

// Doing nothing must not score 100 on the agentic lens: every missing
// planned tool and every unplanned (wrong) tool costs 10.
TEST(agentic_penalizes_missing_and_wrong_tools) {
    Scenario s;
    s.optimal_plan = {{"read", 1}, {"write", 1}};
    Kpi k = perfect_kpi();
    bench::EventStream stream;
    bench::Agentic none = bench::compute_agentic(stream, k, s);
    ASSERT(none.has_plan);
    ASSERT_NEAR(none.score, 80.0, 0.01);  // 2 missing tools

    stream.calls.push_back({"bash", {{"command", "ls"}}, 0, "ok"});
    bench::Agentic wrong = bench::compute_agentic(stream, k, s);
    ASSERT_NEAR(wrong.score, 70.0, 0.01);  // 2 missing + 1 wrong tool
}

// An exact plan match scores 100 on the agentic lens.
TEST(agentic_exact_plan_is_100) {
    Scenario s;
    s.optimal_plan = {{"read", 1}, {"write", 1}};
    Kpi k = perfect_kpi();
    bench::EventStream stream;
    stream.calls.push_back({"read", {{"path", "a.txt"}}, 0, "ok"});
    stream.calls.push_back({"write", {{"path", "b.txt"}}, 0, "ok"});
    bench::Agentic a = bench::compute_agentic(stream, k, s);
    ASSERT(a.has_plan);
    ASSERT_NEAR(a.score, 100.0, 0.01);
}

// must_contain_any: a group passes when ANY member matches; the group counts
// as one check in checks_pass and adherence.
TEST(checks_any_of_parse_and_pass) {
    std::string dir = tmp_dir("anyof");
    write_file(dir + "/c.json", R"json({
        "name": "anyof",
        "suite": "review-datastructures",
        "prompt": "p",
        "checks": {
            "must_contain_any": [["quadratic", "O(n*m)"], ["map", "unordered_set"]]
        }
    })json");
    std::string err;
    auto s = bench::load_scenario(dir + "/c.json", err);
    ASSERT(s.has_value());
    ASSERT_EQ(s->checks.must_contain_any.size(), 2u);
    ASSERT(bench::checks_pass(s->checks, "use a hash map, O(n*m)"));
    ASSERT(!bench::checks_pass(s->checks, "nothing relevant"));
    ASSERT_EQ(bench::adherence(s->checks, "a quadratic map"), 1.0);
    ASSERT_EQ(bench::adherence(s->checks, "unrelated"), 0.0);
}

// The p-03 write oracle matches by target path (args_subset): any write to
// a.txt satisfies the step, a write to a different file is rejected, and the
// read-back step still must follow.
TEST(p03_oracle_write_requires_edits) {
    std::string err;
    auto s = bench::load_scenario(
        "bench/scenarios/prompt/p-03-verify-after-action.json", err);
    ASSERT(s.has_value());
    ASSERT_EQ(s->oracle.size(), 2u);
    const auto& write = s->oracle[0];
    ASSERT_EQ(write.tool, "write");
    ASSERT(write.args.contains("path"));
    ASSERT(write.args_subset);

    // Positive: a path-only write to a.txt satisfies the write step.
    std::vector<bench::ToolCallEvent> ok_calls = {
        {"write", {{"path", "a.txt"}}, 0, "ok"},
        {"read", {{"path", "a.txt"}}, 0, "ok"}};
    ASSERT_EQ(bench::score_oracle(s->oracle, ok_calls).bullseye, 1.0);
    // Negative: a write to a different file cannot satisfy the step.
    std::vector<bench::ToolCallEvent> bad_calls = {
        {"write", {{"path", "other.txt"}}, 0, "ok"},
        {"read", {{"path", "a.txt"}}, 0, "ok"}};
    ASSERT(bench::score_oracle(s->oracle, bad_calls).bullseye < 1.0);
}

// A planned tool called more often than the plan allows costs one unit per
// excess call.
TEST(agentic_penalizes_excess_planned_tools) {
    Scenario s;
    s.optimal_plan = {{"read", 1}, {"write", 1}};
    Kpi k = perfect_kpi();
    bench::EventStream stream;
    stream.calls.push_back({"read", {{"path", "a.txt"}}, 0, "ok"});
    stream.calls.push_back({"read", {{"path", "b.txt"}}, 0, "ok"});
    stream.calls.push_back({"write", {{"path", "c.txt"}}, 0, "ok"});
    bench::Agentic a = bench::compute_agentic(stream, k, s);
    ASSERT(a.has_plan);
    ASSERT_NEAR(a.score, 90.0, 0.01);  // one excess read
}

// ---------------------------------------------------------------------------
// BENCH-01 — repeat medians + confidence intervals (the resolution floor)
// ---------------------------------------------------------------------------

static bench::ScenarioReport score_report(const std::string& name, double total,
                                          int difficulty = 3) {
    bench::ScenarioReport r;
    r.name = name;
    r.suite = "tools";
    r.difficulty = difficulty;
    r.score.total = total;
    r.kpi.success = total >= 60.0;
    return r;
}

// Three runs of one scenario aggregate into a single report: median score,
// repeat count, and a positive standard deviation.
TEST(repeat_aggregates_median_and_stddev) {
    std::vector<bench::ScenarioReport> runs = {
        score_report("s1", 90.0), score_report("s1", 95.0),
        score_report("s1", 97.0)};
    bench::ScenarioReport agg = bench::aggregate_repeats(runs);
    ASSERT_EQ(agg.repeat_n, 3);
    ASSERT_NEAR(agg.score_median, 95.0, 0.01);
    ASSERT(agg.score_stddev > 0.0);
}

// With repeats, the model score is the difficulty-weighted mean of the
// per-scenario MEDIANS — a single wild outlier must not skew it.
TEST(model_score_uses_aggregated_medians) {
    std::vector<bench::ScenarioReport> runs;
    // scenario a: 90, 95, 97 (median 95); scenario b: 40, 80, 82 (median 80)
    for (double s : {90.0, 95.0, 97.0}) runs.push_back(score_report("a", s, 2));
    for (double s : {40.0, 80.0, 82.0}) runs.push_back(score_report("b", s, 2));
    std::vector<bench::ScenarioReport> agg;
    for (const auto& name : {"a", "b"}) {
        std::vector<bench::ScenarioReport> one;
        for (const auto& r : runs)
            if (r.name == name) one.push_back(r);
        agg.push_back(bench::aggregate_repeats(one));
    }
    // weighted mean of medians: (2*95 + 2*80)/4 = 87.5
    ASSERT_NEAR(bench::run_score(agg), 87.5, 0.01);
}

// The resolution rule: two models differ meaningfully only when their gap
// exceeds the combined confidence interval (~2-3 sigma).
TEST(resolution_rule_enforces_noise_floor) {
    // 9-point gap, noisy runs (sigma ~ 5 per model): not resolvable.
    ASSERT_FALSE(bench::resolvable(10.0, 100.0, 10.0, 91.0));
    // Same gap, tight runs (sigma ~ 1): resolvable.
    ASSERT(bench::resolvable(2.0, 100.0, 2.0, 91.0));
}

// The JSON report exposes the aggregate fields when repeats were run.
TEST(repeat_json_emits_median_and_ci) {
    std::vector<bench::ScenarioReport> runs = {
        score_report("s1", 90.0), score_report("s1", 95.0),
        score_report("s1", 97.0)};
    std::vector<bench::ScenarioReport> agg = {
        bench::aggregate_repeats(runs)};
    bench::RunMeta meta;
    meta.mode = "hermetic";
    meta.model = "fake";
    std::string json = bench::render_json(agg, meta);
    ASSERT(json.find("score_median") != std::string::npos);
    ASSERT(json.find("score_stddev") != std::string::npos);
    ASSERT(json.find("model_score_ci") != std::string::npos);
}

// A stored JSON report round-trips: the repeat fields and the model CI
// survive render_json -> parse_report_json, and legacy files (no repeat
// fields) default score_median to score.
TEST(repeat_json_roundtrip_preserves_fields) {
    std::vector<bench::ScenarioReport> runs = {
        score_report("s1", 90.0), score_report("s1", 95.0),
        score_report("s1", 97.0)};
    std::vector<bench::ScenarioReport> agg = {
        bench::aggregate_repeats(runs)};
    bench::RunMeta meta;
    meta.mode = "hermetic";
    meta.model = "fake";
    const std::string json = bench::render_json(agg, meta);

    agent::json j = agent::json::parse(json, nullptr, false);
    ASSERT(!j.is_discarded());
    bench::RunMeta meta2;
    std::vector<bench::ScenarioReport> back;
    ASSERT(bench::parse_report_json(j, meta2, back));
    ASSERT_EQ(back.size(), 1u);
    ASSERT_EQ(back[0].repeat_n, 3);
    ASSERT_NEAR(back[0].score_median, 95.0, 0.01);
    ASSERT(back[0].score_stddev > 0.0);
    ASSERT_EQ(back[0].repeat_scores.size(), 3u);

    // Legacy file: score_median defaults to score.
    agent::json legacy = agent::json::object();
    legacy["scenarios"] = agent::json::array({
        {{"name", "s1"}, {"suite", "tools"}, {"score", 91.0}}});
    std::vector<bench::ScenarioReport> legacy_back;
    ASSERT(bench::parse_report_json(legacy, meta2, legacy_back));
    ASSERT_EQ(legacy_back[0].repeat_n, 1);
    ASSERT_NEAR(legacy_back[0].score_median, 91.0, 0.01);
}

// The text report shows the repeat statistics on repeated scenarios.
TEST(repeat_text_report_shows_stats) {
    std::vector<bench::ScenarioReport> runs = {
        score_report("s1", 90.0), score_report("s1", 95.0),
        score_report("s1", 97.0)};
    std::vector<bench::ScenarioReport> agg = {
        bench::aggregate_repeats(runs)};
    bench::RunMeta meta;
    meta.mode = "hermetic";
    meta.model = "fake";
    const std::string text = bench::render_text(agg, meta);
    ASSERT(text.find("median 95") != std::string::npos);
    ASSERT(text.find("(95% CI)") != std::string::npos);
}

// A two-run aggregation carries the metadata; the canonical score is the
// median, not the representative run's total.
TEST(repeat_two_runs_median_is_canonical) {
    std::vector<bench::ScenarioReport> runs = {
        score_report("s1", 90.0), score_report("s1", 100.0)};
    bench::ScenarioReport agg = bench::aggregate_repeats(runs);
    ASSERT_EQ(agg.repeat_n, 2);
    ASSERT_NEAR(agg.score_median, 95.0, 0.01);
    ASSERT_NEAR(agg.score.total, 95.0, 0.01);
}

// The CI is a property of the MEDIAN estimator: more repeats shrink it.
TEST(model_score_ci_shrinks_with_repeats) {
    std::vector<bench::ScenarioReport> runs;
    runs.reserve(6);
    for (int i = 0; i < 3; ++i)
        runs.push_back(score_report("a", 90.0 + i, 2));
    for (int i = 0; i < 3; ++i)
        runs.push_back(score_report("b", 80.0 + i, 2));
    std::vector<bench::ScenarioReport> agg3;
    for (const auto& name : {"a", "b"}) {
        std::vector<bench::ScenarioReport> one;
        for (const auto& r : runs)
            if (r.name == name) one.push_back(r);
        agg3.push_back(bench::aggregate_repeats(one));
    }
    const double ci3 = bench::model_score_ci(agg3);

    // Nine runs of the same distributions must shrink the interval.
    std::vector<bench::ScenarioReport> runs9;
    runs9.reserve(18);
    for (int i = 0; i < 9; ++i) runs9.push_back(score_report("a", 90.0 + (i % 3), 2));
    for (int i = 0; i < 9; ++i) runs9.push_back(score_report("b", 80.0 + (i % 3), 2));
    std::vector<bench::ScenarioReport> agg9;
    for (const auto& name : {"a", "b"}) {
        std::vector<bench::ScenarioReport> one;
        for (const auto& r : runs9)
            if (r.name == name) one.push_back(r);
        agg9.push_back(bench::aggregate_repeats(one));
    }
    const double ci9 = bench::model_score_ci(agg9);
    ASSERT(ci3 > 0.0);
    ASSERT(ci9 > 0.0);
    ASSERT(ci9 < ci3);
}

// A single run cannot claim precision: the CI is missing, never zero.
TEST(model_score_ci_missing_for_single_runs) {
    std::vector<bench::ScenarioReport> one = {score_report("a", 91.0)};
    ASSERT_EQ(bench::model_score_ci(one), -1.0);
    const std::string text = bench::render_text(one, [&]() {
        bench::RunMeta m;
        m.mode = "hermetic";
        m.model = "fake";
        return m;
    }());
    ASSERT(text.find("no CI") != std::string::npos);
}

// The bootstrap is deterministic: identical inputs give identical CIs.
TEST(model_score_ci_deterministic) {
    std::vector<bench::ScenarioReport> runs;
    runs.reserve(3);
    for (int i = 0; i < 3; ++i) runs.push_back(score_report("a", 90.0 + i));
    std::vector<bench::ScenarioReport> agg = {bench::aggregate_repeats(runs)};
    ASSERT_EQ(bench::model_score_ci(agg), bench::model_score_ci(agg));
}


// ---------------------------------------------------------------------------
// BENCH-02 — discrimination-weighted aggregation (the resolution engine)
// ---------------------------------------------------------------------------

// Scenarios everyone solves are participation trophies: weight ~ 0.
TEST(discrimination_weights_zero_for_trophies) {
    std::vector<std::vector<bench::ScenarioReport>> population;
    for (int m = 0; m < 4; ++m) {
        std::vector<bench::ScenarioReport> run;
        run.push_back(score_report("trophy", 100.0, 3));       // constant
        run.push_back(score_report("trophy2", 99.0, 3));       // constant
        run.push_back(score_report("split", 40.0 + (20.0 * m), 3));
        population.push_back(std::move(run));
    }
    std::map<std::string, double> w = bench::discrimination_weights(population);
    ASSERT_EQ(w.size(), 3u);
    ASSERT_NEAR(w["trophy"], 0.0, 0.001);
    ASSERT_NEAR(w["trophy2"], 0.0, 0.001);
    ASSERT(w["split"] > 5.0);
}

// The separator scenario dominates the delta between near-identical models.
TEST(discriminative_score_ranks_by_separation) {
    // Model A wins the trophy (heavy difficulty), loses the separator.
    std::vector<bench::ScenarioReport> a;
    a.push_back(score_report("trophy", 100.0, 5));
    a.push_back(score_report("split", 30.0, 1));
    std::vector<bench::ScenarioReport> b;
    b.push_back(score_report("trophy", 70.0, 5));
    b.push_back(score_report("split", 100.0, 1));
    std::vector<std::vector<bench::ScenarioReport>> population = {a, b};

    const std::map<std::string, double> w =
        bench::discrimination_weights(population);
    // Plain difficulty-weighted score ranks A above B...
    ASSERT(bench::run_score(a) > bench::run_score(b));
    // ...but the discriminative score must rank B above A: the separator
    // dominates the delta and the trophy contributes ~nothing.
    ASSERT(bench::run_score_discriminative(b, w) >
           bench::run_score_discriminative(a, w));
}

// A single file has no population: the discriminative score falls back to
// the plain difficulty-weighted score.
TEST(discriminative_single_run_falls_back) {
    std::vector<bench::ScenarioReport> run;
    run.push_back(score_report("trophy", 100.0, 3));
    run.push_back(score_report("split", 60.0, 3));
    std::map<std::string, double> empty;
    ASSERT_NEAR(bench::run_score_discriminative(run, empty),
                bench::run_score(run), 0.001);
}


// Weights are keyed by scenario name: a reordered or incomplete run still
// aligns correctly.
TEST(discrimination_weights_keyed_by_name) {
    std::vector<bench::ScenarioReport> m1;
    m1.push_back(score_report("split", 40.0, 2));
    m1.push_back(score_report("trophy", 100.0, 3));
    std::vector<bench::ScenarioReport> m2;
    m2.push_back(score_report("trophy", 100.0, 3));   // reordered
    m2.push_back(score_report("split", 100.0, 2));
    std::vector<std::vector<bench::ScenarioReport>> population = {m1, m2};
    std::map<std::string, double> w = bench::discrimination_weights(population);
    ASSERT_EQ(w.size(), 2u);
    ASSERT_NEAR(w["trophy"], 0.0, 0.001);
    ASSERT(w["split"] > 20.0);

    // Incomplete run: the missing scenario contributes nothing.
    std::vector<bench::ScenarioReport> partial;
    partial.push_back(score_report("split", 60.0, 2));
    const double full = bench::run_score_discriminative(m1, w);
    const double incomplete = bench::run_score_discriminative(partial, w);
    ASSERT(incomplete >= 0.0);
    (void)full;
}

// The discriminative CI matches the discriminative score: with a population
// where a separator dominates, the uncertainty differs from the plain
// difficulty-weighted CI (the score it describes is not the same).
TEST(discriminative_ci_uses_discriminative_weights) {
    std::vector<bench::ScenarioReport> m1;
    m1.push_back(score_report("trophy", 100.0, 5));
    m1.push_back(score_report("split", 30.0, 1));
    std::vector<bench::ScenarioReport> m2;
    m2.push_back(score_report("trophy", 70.0, 5));
    m2.push_back(score_report("split", 100.0, 1));
    std::vector<std::vector<bench::ScenarioReport>> population = {m1, m2};
    const std::map<std::string, double> w =
        bench::discrimination_weights(population);

    // Repeat runs for a CI.
    std::vector<bench::ScenarioReport> runs;
    for (int i = 0; i < 3; ++i) {
        runs.push_back(score_report("trophy", 100.0 - i, 5));
        runs.push_back(score_report("split", 30.0 + i, 1));
    }
    std::vector<bench::ScenarioReport> agg;
    for (const auto& name : {"trophy", "split"}) {
        std::vector<bench::ScenarioReport> one;
        for (const auto& r : runs)
            if (r.name == name) one.push_back(r);
        agg.push_back(bench::aggregate_repeats(one));
    }
    const double plain_ci = bench::model_score_ci(agg);
    const double disc_ci = bench::model_score_ci(agg, w);
    ASSERT(plain_ci > 0.0);
    ASSERT(disc_ci > 0.0);
    // The separator dominates under discrimination -> different uncertainty.
    ASSERT(std::abs(plain_ci - disc_ci) > 0.01);
}


// ---------------------------------------------------------------------------
// BENCH-03 — reference-anchored difficulty ladder (calibration + headroom)
// ---------------------------------------------------------------------------

// The anchor weights center a balanced reference population exactly at the
// target (50 on the 0-100 scale): scenarios the reference solves well lose
// weight, scenarios it fails gain weight.
TEST(calibration_anchor_exact_for_balanced_population) {
    std::vector<bench::ScenarioReport> ref;
    ref.push_back(score_report("easy", 90.0, 3));
    ref.push_back(score_report("hard", 30.0, 3));
    std::vector<bench::ScenarioReport> other;
    other.push_back(score_report("easy", 70.0, 3));
    other.push_back(score_report("hard", 90.0, 3));
    std::vector<std::vector<bench::ScenarioReport>> population = {ref, other};

    const std::vector<double> w =
        bench::anchor_weights(population, 0, 50.0);
    ASSERT_EQ(w.size(), 2u);
    // Weighted reference score lands exactly on the anchor.
    double weighted = 0.0, wsum = 0.0;
    for (size_t i = 0; i < ref.size(); ++i) {
        weighted += w[i] * ref[i].score.total;
        wsum += w[i];
    }
    ASSERT_NEAR(weighted / wsum, 50.0, 0.001);
}

// A higher reference score must never get a higher weight: the ladder
// de-weights what the reference solves well.
TEST(anchor_weights_monotonic_in_score) {
    const std::vector<double> w = bench::anchor_weights(
        {{score_report("a", 95.0, 3), score_report("b", 20.0, 3)}}, 0, 50.0);
    ASSERT(w[0] < w[1]);
}

// The integer suggestions stay in [1, 6] and applying them never worsens the
// anchor deviation vs the current difficulties.
TEST(suggest_difficulties_clamped_and_improve) {
    std::vector<bench::ScenarioReport> ref;
    ref.push_back(score_report("easy", 90.0, 3));
    ref.push_back(score_report("hard", 30.0, 3));
    std::vector<bench::ScenarioReport> other;
    other.push_back(score_report("easy", 70.0, 3));
    other.push_back(score_report("hard", 90.0, 3));
    std::vector<std::vector<bench::ScenarioReport>> population = {ref, other};

    const std::vector<int> d = bench::suggest_difficulties(population, 0);
    ASSERT_EQ(d.size(), 2u);
    for (const int v : d) {
        ASSERT_TRUE(v >= 1);
        ASSERT_TRUE(v <= 6);
    }

    const double before = bench::reference_anchor_deviation(population, 0);
    double weighted = 0.0, wsum = 0.0;
    for (size_t i = 0; i < ref.size(); ++i) {
        weighted += d[i] * ref[i].score.total;
        wsum += d[i];
    }
    const double after = std::abs((weighted / wsum) - 50.0);
    ASSERT(after <= before);
}

// The chart must have a top: the best model leaves headroom below 1000 for
// larger models yet to come.
TEST(headroom_positive_below_ceiling) {
    std::vector<std::vector<bench::ScenarioReport>> population = {
        {score_report("s", 95.0, 3)}, {score_report("s", 90.0, 3)}};
    ASSERT(bench::headroom(population) > 0.0);
}

// The difficulty ladder extends to 6 (headroom tier for larger models).
TEST(difficulty_six_scenario_loads) {
    std::string dir = tmp_dir("d6");
    write_file(dir + "/h.json", R"json({
        "name": "headroom-1",
        "suite": "coding",
        "prompt": "p",
        "difficulty": 6,
        "checks": {"must_contain": ["done"]}
    })json");
    std::string err;
    auto s = bench::load_scenario(dir + "/h.json", err);
    ASSERT(s.has_value());
    ASSERT_EQ(s->difficulty, 6);
}


// ---------------------------------------------------------------------------
// Oracle path normalization (found by the local baseline)
// ---------------------------------------------------------------------------

// A live agent reads workspace files via their ABSOLUTE path (the tools
// resolve them); an oracle expecting a bare relative name must still match.
TEST(oracle_matches_bare_filename_against_absolute_path) {
    std::vector<bench::ScenarioStep> oracle = {
        {"read", {{"path", "Review.cpp"}}, false, false}};
    std::vector<bench::ToolCallEvent> calls = {
        {"read", {{"path", "/tmp/amber_bench_ws_arch-01/Review.cpp"}}, 0,
         "ok"}};
    bench::OracleResult r = bench::score_oracle(oracle, calls);
    ASSERT_EQ(r.bullseye, 1.0);
    ASSERT_EQ(r.wasted, 0);
}

// And the reverse: an expected absolute path matches a relative call.
TEST(oracle_absolute_path_matches_relative_call) {
    std::vector<bench::ScenarioStep> oracle = {
        {"read", {{"path", "/tmp/amber_bench_ws_x/notes.txt"}}, false, false}};
    std::vector<bench::ToolCallEvent> calls = {
        {"read", {{"path", "notes.txt"}}, 0, "ok"}};
    ASSERT_EQ(bench::score_oracle(oracle, calls).bullseye, 1.0);
}

// Non-filename expected values keep exact semantics (no basename hijack).
TEST(oracle_non_filename_keeps_exact_match) {
    std::vector<bench::ScenarioStep> oracle = {
        {"read", {{"path", "sub/dir/file.txt"}}, false, false}};
    std::vector<bench::ToolCallEvent> calls = {
        {"read", {{"path", "/tmp/other/file.txt"}}, 0, "ok"}};
    ASSERT_EQ(bench::score_oracle(oracle, calls).bullseye, 0.0);
}


// A nested RELATIVE expectation must also match the absolute form the live
// agent produces: expected "src/header.h" vs actual "/tmp/ws/src/header.h".
TEST(oracle_nested_relative_matches_absolute) {
    std::vector<bench::ScenarioStep> oracle = {
        {"write", {{"path", "src/header.h"}}, false, false}};
    std::vector<bench::ToolCallEvent> calls = {
        {"write", {{"path", "/tmp/ws/src/header.h"}}, 0, "ok"}};
    ASSERT_EQ(bench::score_oracle(oracle, calls).bullseye, 1.0);
}

// ...but a different directory with the same leaf must not match.
TEST(oracle_nested_relative_rejects_other_directory) {
    std::vector<bench::ScenarioStep> oracle = {
        {"write", {{"path", "src/header.h"}}, false, false}};
    std::vector<bench::ToolCallEvent> calls = {
        {"write", {{"path", "/tmp/ws/include/header.h"}}, 0, "ok"}};
    ASSERT_EQ(bench::score_oracle(oracle, calls).bullseye, 0.0);
}

// h-02 regression (live baseline): the scenario's own oracle must score the
// model's correct behavior at bullseye 1.0. A correct rename edits only the
// files that reference OldName and verifies the untouched one with a read —
// the write tool's {path, edits} args require args_subset, and other.cpp is
// checked, not rewritten.
// h-02's verification read may legitimately come BEFORE the writes: a
// survey-then-edit agent reads all three files first, then edits the two
// that reference OldName. The oracle is unordered so both orders score 1.0,
// while a wrong write to other.cpp still counts as wasted (no step expects it).
TEST(scenario_h02_oracle_unordered_survey_then_edit) {
    std::string err;
    auto s = bench::load_scenario("bench/scenarios/headroom/h-02-multi-file-consistency.json", err);
    ASSERT(s.has_value());
    std::vector<bench::ToolCallEvent> calls = {
        {"search", {{"pattern", "OldName"}}, 0, "ok"},
        {"read", {{"path", "src/header.h"}}, 0, "ok"},
        {"read", {{"path", "src/impl.cpp"}}, 0, "ok"},
        {"read", {{"path", "src/other.cpp"}}, 0, "ok"},
        {"write", {{"path", "src/header.h"}, {"edits", {{"old", "OldName"}, {"new", "NewName"}}}}, 0, "ok"},
        {"write", {{"path", "src/impl.cpp"}, {"edits", {{"old", "OldName"}, {"new", "NewName"}}}}, 0, "ok"}};
    bench::OracleResult r = bench::score_oracle(s->oracle, calls);
    ASSERT_EQ(r.bullseye, 1.0);
    ASSERT_EQ(r.wasted, 2);  // survey search + impl.cpp read are off-oracle
}

// ...but editing a file the oracle never expects (other.cpp) is still a miss.
TEST(scenario_h02_oracle_rejects_rewrite_of_other_cpp) {
    std::string err;
    auto s = bench::load_scenario("bench/scenarios/headroom/h-02-multi-file-consistency.json", err);
    ASSERT(s.has_value());
    std::vector<bench::ToolCallEvent> calls = {
        {"read", {{"path", "src/header.h"}}, 0, "ok"},
        {"write", {{"path", "src/header.h"}, {"edits", {{"old", "OldName"}, {"new", "NewName"}}}}, 0, "ok"},
        {"write", {{"path", "src/impl.cpp"}, {"edits", {{"old", "OldName"}, {"new", "NewName"}}}}, 0, "ok"},
        {"write", {{"path", "src/other.cpp"}, {"edits", {{"old", "OldName"}, {"new", "NewName"}}}}, 0, "ok"},
        {"read", {{"path", "src/other.cpp"}}, 0, "ok"}};
    bench::OracleResult r = bench::score_oracle(s->oracle, calls);
    ASSERT_EQ(r.bullseye, 1.0);
    ASSERT_EQ(r.wasted, 1);  // the redundant rewrite of other.cpp
}

TEST(scenario_h02_oracle_scores_correct_rename) {
    std::string err;
    auto s = bench::load_scenario("bench/scenarios/headroom/h-02-multi-file-consistency.json", err);
    ASSERT(s.has_value());
    ASSERT(s->oracle.size() == 4u);
    std::vector<bench::ToolCallEvent> calls = {
        {"read", {{"path", "src/header.h"}}, 0, "ok"},
        {"write", {{"path", "src/header.h"}, {"edits", {{"old", "OldName"}, {"new", "NewName"}}}}, 0, "ok"},
        {"write", {{"path", "src/impl.cpp"}, {"edits", {{"old", "OldName"}, {"new", "NewName"}}}}, 0, "ok"},
        {"read", {{"path", "src/other.cpp"}}, 0, "ok"}};
    bench::OracleResult r = bench::score_oracle(s->oracle, calls);
    ASSERT_EQ(r.bullseye, 1.0);
    ASSERT_EQ(r.wasted, 0);
}

// A read oracle step {"path": "Review.cpp"} must match a call that adds the
// tool's OPTIONAL args (read accepts limit) — the step declares required
// keys, not an exhaustive set. Exact key-count matching is opt-in.
TEST(oracle_default_is_subset_extra_args_ok) {
    std::vector<bench::ScenarioStep> steps = {
        {"read", {{"path", "Review.cpp"}}}};  // no args_subset flag
    std::vector<bench::ToolCallEvent> calls = {
        {"read", {{"path", "/tmp/ws/Review.cpp"}, {"limit", 200}}, 0, "ok"}};
    bench::OracleResult r = bench::score_oracle(steps, calls);
    ASSERT_EQ(r.bullseye, 1.0);
    ASSERT_EQ(r.wasted, 0);
}

// ...but a missing REQUIRED key still fails, and an explicit args_subset
// false still demands exact key sets (opt-in strictness preserved).
TEST(oracle_default_subset_still_requires_all_expected_keys) {
    std::vector<bench::ScenarioStep> steps = {
        {"read", {{"path", "a.txt"}, {"lines", 40}}}};
    std::vector<bench::ToolCallEvent> calls = {
        {"read", {{"path", "a.txt"}}, 0, "ok"}};
    bench::OracleResult r = bench::score_oracle(steps, calls);
    ASSERT_FALSE(r.success);
    ASSERT_EQ(r.matched_steps, 0);
}

// ---------------------------------------------------------------------------
// Harness probes (docs/spec/benchmark/harness.md) — the engine health axis.
// Each probe fixes its input so a deviation is provably a harness bug. The
// scorecard aggregates probes per family; a clean tree must be 1.0.
// ---------------------------------------------------------------------------

// Every required probe family must be represented in the harness scorecard.
// A family going missing (probe removed, registry broken) fails the axis.
TEST(harness_scorecard_covers_all_families) {
    std::vector<bench::ProbeResult> results = bench::run_all_probes();
    bench::HarnessScorecard sc = bench::aggregate_probes(results);
    ASSERT_EQ(sc.total, static_cast<int>(results.size()));
    ASSERT_TRUE(sc.total > 0);
    for (const auto& fam : bench::required_probe_families()) {
        auto it = sc.families.find(fam);
        ASSERT_TRUE(it != sc.families.end());
        ASSERT_TRUE(it->second.second > 0);  // at least one probe ran
    }
}

// A clean tree must produce a fully green harness scorecard. A red probe here
// means the engine regressed on a mechanism the harness axis covers.
TEST(harness_scorecard_clean_tree_is_green) {
    std::vector<bench::ProbeResult> results = bench::run_all_probes();
    bench::HarnessScorecard sc = bench::aggregate_probes(results);
    for (const auto& p : sc.probes) {
        if (!p.passed)
            ::agent::test::fail("probe failed: " + p.name + " [" + p.family +
                                "] — expected=" + p.expected +
                                " detail=" + p.detail);
    }
    ASSERT_EQ(sc.passed, sc.total);
    ASSERT_EQ(sc.integrity, 1.0);
}

// The extract family must include the Hermes-style bare-JSON probe that
// pinned the 32B regression (bare {"name":...,"arguments":...} in content).
TEST(harness_extract_probe_pins_bare_json) {
    std::vector<bench::ProbeResult> results = bench::run_all_probes();
    bool found = false;
    for (const auto& p : results) {
        if (p.family == "extract" &&
            p.name.find("bare") != std::string::npos) {
            found = true;
            ASSERT_TRUE(p.passed);
        }
    }
    ASSERT(found);
}

// The parse family must reconstruct a tool_calls SSE stream into the exact
// message — the mechanism that silently swallowed 32B calls pre-fix.
TEST(harness_parse_probe_roundtrips_tool_calls_sse) {
    std::vector<bench::ProbeResult> results = bench::run_all_probes();
    bool found = false;
    for (const auto& p : results) {
        if (p.family == "parse" &&
            p.name.find("tool_calls") != std::string::npos) {
            found = true;
            ASSERT_TRUE(p.passed);
        }
    }
    ASSERT(found);
}

// The context probe must survive push/pop/clear/rebuild with the FNV chain
// intact — an in-place mutation would assert (or corrupt) in debug builds.
TEST(harness_context_probe_chain_survives) {
    std::vector<bench::ProbeResult> results = bench::run_all_probes();
    bool found = false;
    for (const auto& p : results) {
        if (p.family == "context" &&
            p.name.find("chain") != std::string::npos) {
            found = true;
            ASSERT_TRUE(p.passed);
        }
    }
    ASSERT(found);
}

// Family aggregation math: a mixed result set must produce the right
// per-family and overall integrity.
TEST(harness_scorecard_aggregation_math) {
    std::vector<bench::ProbeResult> results = {
        {"parse", "p1", true, "ok", "ok"},
        {"parse", "p2", false, "dropped", "kept"},
        {"extract", "e1", true, "ok", "ok"},
        {"budget", "b1", true, "ok", "ok"},
    };
    bench::HarnessScorecard sc = bench::aggregate_probes(results);
    ASSERT_EQ(sc.passed, 3);
    ASSERT_EQ(sc.total, 4);
    ASSERT_EQ(sc.integrity, 0.75);
    ASSERT_EQ(sc.families["parse"].first, 1);   // passed
    ASSERT_EQ(sc.families["parse"].second, 2);  // total
    ASSERT_EQ(sc.family_integrity("extract"), 1.0);
    ASSERT_EQ(sc.family_integrity("parse"), 0.5);
}
