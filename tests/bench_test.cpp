
#include "bench/fake.h"
#include "bench/kpi.h"
#include "bench/oracle.h"
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
        {"read", {{"path", "a.txt"}}},
        {"write", {{"path", "b.txt"}}},
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
    std::vector<ToolCallEvent> calls = {{"read", {{"path", "src/a.txt"}}}};
    OracleResult r = bench::score_oracle(steps, calls);
    ASSERT(r.success);
    ASSERT_EQ(r.bullseye, 1.0);
}

TEST(oracle_args_subset_relaxes) {
    std::vector<ScenarioStep> steps = {
        {"read", {{"path", "a.txt"}}, true},
    };
    std::vector<ToolCallEvent> calls = {{"read", {{"path", "a.txt"}, {"lines", 40}}}};
    OracleResult r = bench::score_oracle(steps, calls);
    ASSERT(r.success);
    ASSERT_EQ(r.arg_precision, 1.0);
}

TEST(oracle_missing_expected_key_fails) {
    std::vector<ScenarioStep> steps = {
        {"read", {{"path", "a.txt"}, {"lines", 40}}, false},
    };
    std::vector<ToolCallEvent> calls = {{"read", {{"path", "a.txt"}}}};
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
        {"search", {{"query", "x"}}},
        {"read", {{"path", "a.txt"}}},
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
    std::vector<ToolCallEvent> calls = {{"read", {{"path", "a.txt"}}}};
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
        {"read", {{"path", "a.txt"}}},
        {"bash", {{"command", "ls"}}},
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
        {"read", {{"path", "a.txt"}}},
        {"read", {{"path", "a.txt"}}},
        {"read", {{"path", "b.txt"}}},
    };
    OracleResult r = bench::score_oracle(steps, calls);
    ASSERT(r.success);
    ASSERT_EQ(r.redundant, 1);
    ASSERT_EQ(r.wasted, 1);
}

TEST(oracle_empty_oracle_success) {
    OracleResult r = bench::score_oracle({}, {{"read", {{"path", "a.txt"}}}});
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
    hooks.on_tool_result("read", agent::ToolResult{true, "hello", "", agent::json{}});
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
    hooks.on_tool_result("search", agent::ToolResult{true, "hit", "", agent::json{}});
    hooks.on_tool_result("read", agent::ToolResult{true, "text", "", agent::json{}});
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
    hooks.on_tool_result("bash", agent::ToolResult{false, "", "denied", agent::json{{"denied", true}}});
    hooks.on_tool_call("read", {{"path", "a.txt"}});
    hooks.on_tool_result("read", agent::ToolResult{false, "", "timeout", agent::json{{"timeout", true}}});
    ASSERT_EQ(rec.stream().tools.size(), 2u);
    ASSERT(rec.stream().tools[0].denied);
    ASSERT(rec.stream().tools[1].timeout);
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

int main() { return agent::test::run_all(); }
