
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "agent.h"
#include "agent/version.h"
#include "bench/probe.h"
#include "bench/report.h"
#include "bench/runner.h"
#include "bench/scenario.h"

namespace fs = std::filesystem;

namespace {

void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " <command> [options]\n\n"
              << "Commands:\n"
              << "  list                        list available scenarios\n"
              << "  run                         run scenarios (hermetic fake LLM by default)\n"
              << "  validate-template <scenario> prove a coding template's hidden tests pass\n"
              << "  report <results.json>...     re-render stored JSON report(s);\n"
              << "                               multiple files render a model comparison\n"
              << "  delta <a.json> <b.json>      win/lose/stagnate per-scenario KPI delta\n"
              << "  scorecard <results.json>     full diagnostic: dimensions, per-suite,\n"
              << "                               failed-scenario diagnosis, signals\n"
              << "  calibrate <results.json>...  reference-anchored calibration: headroom, anchor\n"
              << "                               deviation, suggested difficulties\n\n"
              << "Options:\n"
              << "  --suite NAME     filter by suite\n"
              << "  --scenario NAME  run a single scenario\n"
              << "  --live           use the real model (amber.conf / env / flags)\n"
              << "  --profile NAME   model profile from bench/profiles.json\n"
              << "  --model NAME     model override (live)\n"
              << "  --temperature T  temperature override (live)\n"
              << "  --repeat N       run each scenario N times\n"
              << "  --cat NAME       category: model | harness | both (default)\n"
              << "  --out FILE       write JSON report to FILE\n"
              << "  --format FMT     report output: text (default), markdown, json\n"
              << "  -h, --help       show this help\n";
}

std::vector<bench::Scenario> discover_scenarios(const std::string& suite, const std::string& name) {
    std::vector<bench::Scenario> out;
    const fs::path root = fs::current_path() / "bench" / "scenarios";
    if (!fs::is_directory(root)) {
        std::cerr << "error: no bench/scenarios directory in " << fs::current_path().string()
                  << "\n";
        return out;
    }
    std::vector<fs::path> files;
    for (const auto& e : fs::recursive_directory_iterator(root))
        if (e.is_regular_file() && e.path().extension() == ".json") {
            // Scenarios live at bench/scenarios/<suite>/<name>.json; template
            // metadata (checks.json) sits deeper and is not a scenario.
            const fs::path rel = fs::relative(e.path(), root);
            if (std::distance(rel.begin(), rel.end()) == 2)
                files.push_back(e.path());
        }
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
        std::string err;
        auto s = bench::load_scenario(f.string(), err);
        if (!s) {
            std::cerr << "warning: " << f.string() << ": " << err << "\n";
            continue;
        }
        if (!suite.empty() && s->suite != suite)
            continue;
        if (!name.empty() && s->name != name)
            continue;
        out.push_back(std::move(*s));
    }
    return out;
}

void apply_profile(bench::RunOptions& opts, const std::string& profile) {
    if (profile.empty())
        return;
    std::ifstream f("bench/profiles.json");
    if (!f) {
        std::cerr << "error: bench/profiles.json not found\n";
        return;
    }
    agent::json j;
    try {
        j = agent::json::parse(f);
    } catch (...) {
        std::cerr << "error: bench/profiles.json is not valid JSON\n";
        return;
    }
    if (!j.contains(profile) || !j[profile].is_object()) {
        std::cerr << "error: unknown profile \"" << profile << "\"\n";
        return;
    }
    const agent::json& p = j[profile];
    if (p.contains("model") && p["model"].is_string())
        opts.model = p["model"].get<std::string>();
    if (p.contains("temperature") && p["temperature"].is_number())
        opts.temperature = p["temperature"].get<double>();
    if (p.contains("thinking") && p["thinking"].is_string())
        opts.thinking = p["thinking"].get<std::string>();
    if (p.contains("thinking_budget") && p["thinking_budget"].is_number_integer())
        opts.thinking_budget = p["thinking_budget"].get<int>();
}

std::string run_id() {
    std::string id = "run-";
    id += std::to_string(
        static_cast<long long>(std::chrono::system_clock::now().time_since_epoch().count()));
    return id;
}

// The whole command line: the options every subcommand reads, plus the trailing
// arguments none of them claimed.
struct CliOptions {
    bool live = false;
    std::string suite;
    std::string name;
    std::string profile;
    std::string model;
    double temperature = -1;
    int repeat = 1;
    std::string out_file;
    std::string debug_dir;
    std::string category;
    std::string disable;
    std::string format = "text";
    std::vector<std::string> rest;
};

using NextArg = std::function<std::string(const char*)>;

// Flags that consume the next argument.
bool apply_value_flag(const std::string& a, const NextArg& next, CliOptions& o) {
    if (a == "--suite")
        o.suite = next("");
    else if (a == "--scenario")
        o.name = next("");
    else if (a == "--profile")
        o.profile = next("");
    else if (a == "--model")
        o.model = next("");
    else if (a == "--temperature")
        o.temperature = std::atof(next("0").c_str());
    else if (a == "--repeat")
        o.repeat = std::atoi(next("1").c_str());
    else if (a == "--debug")
        o.debug_dir = next("");
    else if (a == "--disable")
        o.disable = next("");
    else if (a == "--out")
        o.out_file = next("");
    else if (a == "--format")
        o.format = next("text");
    else if (a == "--cat" || a == "--category")
        o.category = next("both");
    else
        return false;
    return true;
}

// Parse the command line after the subcommand. Returns 0 when a flag asked to
// stop (-h/--help), otherwise nullopt.
std::optional<int> parse_cli(int argc, char** argv, CliOptions& o) {
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* def) -> std::string {
            if (i + 1 < argc)
                return argv[++i];
            return def;
        };
        if (a == "--live") {
            o.live = true;
        } else if (a == "-h" || a == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (!apply_value_flag(a, next, o)) {
            o.rest.push_back(a);
        }
    }
    return std::nullopt;
}

// The plugins named by --disable, comma separated.
std::vector<std::string> parse_disable_list(const std::string& disable) {
    std::vector<std::string> out;
    std::stringstream ids(disable);
    std::string id;
    while (std::getline(ids, id, ','))
        if (!id.empty())
            out.push_back(id);
    return out;
}

bench::RunOptions build_run_options(const CliOptions& args) {
    bench::RunOptions opts;
    opts.disable_plugins = parse_disable_list(args.disable);
    opts.live = args.live;
    opts.repeat = args.repeat;
    opts.model = args.model;
    opts.temperature = args.temperature;
    opts.debug_dir = args.debug_dir;
    apply_profile(opts, args.profile);
    return opts;
}

bench::RunMeta build_run_meta(const CliOptions& args, const bench::RunOptions& opts) {
    bench::RunMeta meta;
    meta.run_id = run_id();
    meta.mode = args.live ? "live" : "hermetic";
    meta.profile = args.profile;
    meta.model = opts.model;
    if (meta.model.empty())
        meta.model = args.live ? "config" : "fake";
    meta.engine_version = agent::kVersion;
    meta.timestamp = std::to_string(
        static_cast<long long>(std::chrono::system_clock::now().time_since_epoch().count()));
    meta.reasoning = opts.thinking.empty() ? "auto" : opts.thinking;
    return meta;
}

// The probe scorecard. `leading_blank` separates it from a model axis printed
// before it.
void print_harness_scorecard(const std::vector<bench::ProbeResult>& probes,
                             const bench::HarnessScorecard& sc, bool leading_blank) {
    if (leading_blank)
        std::cout << "\n";
    std::cout << "## Harness scorecard\n\n";
    for (const auto& fam : bench::required_probe_families()) {
        std::cout << "  " << fam << " " << sc.families.at(fam).first << "/"
                  << sc.families.at(fam).second << "\n";
    }
    std::cout << "\n  integrity " << sc.integrity << " (" << sc.passed << "/" << sc.total
              << ")\n\n";
    for (const auto& p : probes) {
        std::cout << (p.passed ? "[ PASS ] " : "[ FAIL ] ") << p.family << "/" << p.name << "\n";
        if (!p.passed)
            std::cout << "          expected=" << p.expected << " detail=" << p.detail << "\n";
    }
}

// The harness JSON sidecar. `with_probes` is true on the harness-only path,
// where the per-probe detail is the only output and the file is newline
// terminated; the combined run writes the model axis separately.
void write_harness_json(const std::string& out_file, const std::vector<bench::ProbeResult>& probes,
                        const bench::HarnessScorecard& sc, bool with_probes) {
    if (out_file.empty())
        return;
    agent::json out;
    out["harness_integrity"] = sc.integrity;
    out["harness_passed"] = sc.passed;
    out["harness_total"] = sc.total;
    agent::json fam;
    for (const auto& f : sc.families)
        fam[f.first] = {{"passed", f.second.first}, {"total", f.second.second}};
    out["harness_families"] = fam;
    if (with_probes) {
        agent::json plist = agent::json::array();
        for (const auto& p : probes) {
            agent::json pj;
            pj["family"] = p.family;
            pj["name"] = p.name;
            pj["passed"] = p.passed;
            pj["detail"] = p.detail;
            pj["expected"] = p.expected;
            pj["ms"] = p.ms;
            plist.push_back(std::move(pj));
        }
        out["probes"] = std::move(plist);
    }
    std::ofstream fout(out_file);
    fout << out.dump(2);
    if (with_probes)
        fout << "\n";
}

// Harness-only: the deterministic engine-health scorecard.
int run_harness_axis(const std::string& out_file) {
    const auto probes = bench::run_all_probes();
    const bench::HarnessScorecard sc = bench::aggregate_probes(probes);
    print_harness_scorecard(probes, sc, /*leading_blank=*/false);
    write_harness_json(out_file, probes, sc, /*with_probes=*/true);
    return sc.passed == sc.total ? 0 : 1;
}

// The model axis: run the scenarios and print the text report, with the JSON
// sidecar when asked for.
int run_model_axis(const CliOptions& args) {
    const bench::RunOptions opts = build_run_options(args);
    auto scenarios = discover_scenarios(args.suite, args.name);
    if (scenarios.empty()) {
        std::cerr << "error: no scenarios match\n";
        return 1;
    }
    bench::RunMeta meta = build_run_meta(args, opts);
    const std::vector<bench::ScenarioReport> reports = bench::run_scenarios(scenarios, opts, meta);
    std::cout << bench::render_text(reports, meta);
    if (!args.out_file.empty()) {
        std::ofstream out(args.out_file);
        out << bench::render_json(reports, meta);
    }
    return 0;
}

// Both: the model axis first, then the harness axis. A run with no matching
// scenarios still reports the harness rather than failing.
int run_both_axes(const CliOptions& args) {
    const bench::RunOptions opts = build_run_options(args);
    auto scenarios = discover_scenarios(args.suite, args.name);
    bench::RunMeta meta = build_run_meta(args, opts);
    if (scenarios.empty()) {
        std::cerr << "warning: no scenarios match; harness axis only\n";
    } else {
        const std::vector<bench::ScenarioReport> reports =
            bench::run_scenarios(scenarios, opts, meta);
        std::cout << bench::render_text(reports, meta);
    }

    const auto probes = bench::run_all_probes();
    const bench::HarnessScorecard sc = bench::aggregate_probes(probes);
    print_harness_scorecard(probes, sc, /*leading_blank=*/true);
    write_harness_json(args.out_file, probes, sc, /*with_probes=*/false);
    return sc.passed == sc.total ? 0 : 1;
}

int cmd_run(const CliOptions& args) {
    if (!args.category.empty() && args.category != "model" && args.category != "harness" &&
        args.category != "both") {
        std::cerr << "error: unknown category \"" << args.category
                  << "\" (expected model | harness | both)\n";
        return 1;
    }
    if (args.category == "harness")
        return run_harness_axis(args.out_file);
    if (args.category == "both")
        return run_both_axes(args);
    return run_model_axis(args); // "" or "model"
}

int cmd_list(const std::string& suite) {
    auto scenarios = discover_scenarios(suite, "");
    for (const auto& s : scenarios) {
        std::cout << s.suite << "/" << s.name << (s.hermetic_only ? " [hermetic]" : "")
                  << (s.template_dir.empty() ? "" : " [template]") << "\n";
        if (!s.description.empty())
            std::cout << "    " << s.description << "\n";
    }
    return 0;
}

int cmd_validate(const std::string& name) {
    auto scenarios = discover_scenarios("", name);
    if (scenarios.size() != 1) {
        std::cerr << "error: scenario not found (or ambiguous): " << name << "\n";
        return 1;
    }
    const bench::Scenario& s = scenarios[0];
    if (s.template_dir.empty()) {
        std::cerr << "error: scenario has no template: " << name << "\n";
        return 1;
    }
    const fs::path tpl = fs::current_path() / "bench" / "scenarios" / s.template_dir;
    std::string err;
    bench::TemplateResult r =
        bench::run_template(tpl.string(), (tpl / "reference").string(), "g++", err);
    std::cout << "template " << s.name << ": " << r.tests_passed << "/" << r.tests_total
              << " hidden tests pass" << (r.compile_ok ? "" : " (compile failed)") << "\n";
    if (!err.empty())
        std::cerr << "error: " << err << "\n";
    return (r.compile_ok && r.tests_passed == r.tests_total) ? 0 : 1;
}

namespace {
// Load and decode one stored report file; nullopt with a message on failure.
std::optional<agent::json> load_report_json(const std::string& file, std::string& err) {
    std::ifstream f(file);
    if (!f) {
        err = "cannot open " + file;
        return std::nullopt;
    }
    try {
        return agent::json::parse(f);
    } catch (const std::exception& e) {
        err = file + ": " + e.what();
        return std::nullopt;
    }
}
} // namespace

bool parse_report_file(const std::string& file, bench::RunMeta& meta,
                       std::vector<bench::ScenarioReport>& reports) {
    std::string err;
    auto j = load_report_json(file, err);
    if (!j) {
        std::cerr << "error: " << err << "\n";
        return false;
    }
    try {
        if (!bench::parse_report_json(*j, meta, reports)) {
            std::cerr << "error: " << file << " is not an amber-bench report\n";
            return false;
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << file << ": " << e.what() << "\n";
        return false;
    }
    return true;
}

int cmd_report(const std::vector<std::string>& files, const std::string& format) {
    if (files.empty()) {
        std::cerr << "error: report needs at least one results file\n";
        return 1;
    }
    std::vector<std::pair<bench::RunMeta, std::vector<bench::ScenarioReport>>> runs;
    for (const auto& f : files) {
        bench::RunMeta meta;
        std::vector<bench::ScenarioReport> reports;
        if (!parse_report_file(f, meta, reports))
            return 1;
        runs.emplace_back(std::move(meta), std::move(reports));
    }
    if (format == "markdown") {
        if (runs.size() == 1)
            std::cout << bench::render_markdown(runs[0].second, runs[0].first);
        else
            std::cout << bench::render_markdown_comparison(runs);
    } else if (format == "json") {
        std::cout << bench::render_json(runs[0].second, runs[0].first);
    } else {
        for (const auto& run : runs)
            std::cout << bench::render_text(run.second, run.first) << "\n";
    }
    return 0;
}

enum class DeltaVerdict { Win, Lose, Same };

DeltaVerdict delta_verdict(double dscore) {
    if (dscore > 1.0)
        return DeltaVerdict::Win;
    if (dscore < -1.0)
        return DeltaVerdict::Lose;
    return DeltaVerdict::Same;
}

const char* delta_verdict_name(DeltaVerdict v) {
    switch (v) {
    case DeltaVerdict::Win:
        return "WIN";
    case DeltaVerdict::Lose:
        return "LOSE";
    case DeltaVerdict::Same:
        break;
    }
    return "same";
}

// Running totals for the delta footer.
struct DeltaTotals {
    double a = 0.0;
    double b = 0.0;
    int win = 0;
    int lose = 0;
    int same = 0;
};

void print_delta_header() {
    std::cout << std::string(78, '-') << "\n";
    std::cout << "scenario                      old     new   dScore "
                 "dWasted dRedund dFail  dSteps  verdict\n";
    std::cout << std::string(78, '-') << "\n";
}

// Index the reports by scenario name so two runs can be paired.
std::map<std::string, const bench::ScenarioReport*>
index_by_name(const std::vector<bench::ScenarioReport>& reports) {
    std::map<std::string, const bench::ScenarioReport*> out;
    for (const auto& r : reports)
        out[r.name] = &r;
    return out;
}

// One scenario's delta row, folding its verdict into the running totals.
void print_delta_row(const std::string& name, const bench::ScenarioReport& ra,
                     const bench::ScenarioReport& rb, DeltaTotals& t) {
    const double da = ra.score.total;
    const double db = rb.score.total;
    t.a += da;
    t.b += db;
    const double dscore = db - da;
    const DeltaVerdict verdict = delta_verdict(dscore);
    if (verdict == DeltaVerdict::Win)
        ++t.win;
    else if (verdict == DeltaVerdict::Lose)
        ++t.lose;
    else
        ++t.same;
    printf("%-30s %6.1f %6.1f %+6.1f %+7d %+7d %+5d %+7d  %s\n", name.c_str(), da, db, dscore,
           rb.kpi.wasted - ra.kpi.wasted, rb.kpi.redundant - ra.kpi.redundant,
           rb.kpi.tool_failures - ra.kpi.tool_failures, rb.kpi.steps - ra.kpi.steps,
           delta_verdict_name(verdict));
}

int cmd_delta(const std::vector<std::string>& files) {
    // win/lose/stagnate between two stored runs, per scenario.
    if (files.size() < 2) {
        std::cerr << "error: delta needs two results files\n";
        return 1;
    }
    bench::RunMeta m1, m2;
    std::vector<bench::ScenarioReport> r1, r2;
    if (!parse_report_file(files[0], m1, r1))
        return 1;
    if (!parse_report_file(files[1], m2, r2))
        return 1;

    const auto a = index_by_name(r1);
    const auto b = index_by_name(r2);
    std::cout << "# Delta: " << files[0] << " -> " << files[1] << "\n\n";
    DeltaTotals t;
    print_delta_header();
    for (const auto& [name, ra] : a) {
        const auto it = b.find(name);
        if (it == b.end())
            continue;
        print_delta_row(name, *ra, *it->second, t);
    }
    std::cout << std::string(78, '-') << "\n";
    printf("%-30s %6.1f %6.1f %+6.1f\n", "TOTAL", t.a, t.b, t.b - t.a);
    std::cout << "\nwin " << t.win << " | lose " << t.lose << " | same " << t.same << "\n";
    return 0;
}

} // namespace

namespace {
// Reference-anchored calibration table (BENCH-03): per-model scores, the
// anchor deviation, the headroom, and per-scenario difficulty suggestions.
// The reference defaults to the best model in the population.
int cmd_calibrate(const std::vector<std::string>& files) {
    std::vector<std::pair<bench::RunMeta, std::vector<bench::ScenarioReport>>> runs;
    for (const auto& f : files) {
        bench::RunMeta meta;
        std::vector<bench::ScenarioReport> reports;
        if (!parse_report_file(f, meta, reports))
            return 1;
        runs.emplace_back(std::move(meta), std::move(reports));
    }
    if (runs.empty()) {
        std::cerr << "error: calibrate needs at least one results file\n";
        return 1;
    }
    std::vector<std::vector<bench::ScenarioReport>> population;
    population.reserve(runs.size());
    for (const auto& run : runs)
        population.push_back(run.second);

    size_t reference = 0;
    for (size_t i = 1; i < runs.size(); ++i)
        if (bench::run_score(runs[i].second) > bench::run_score(runs[reference].second))
            reference = i;

    std::cout << "# Calibration (reference: " << runs[reference].first.model << ")\n\n";
    for (const auto& run : runs)
        std::cout << "- " << run.first.model << ": "
                  << static_cast<int>(bench::run_score(run.second) * 10.0) << "/1000\n";
    std::cout << "\nanchor deviation: "
              << static_cast<int>(bench::reference_anchor_deviation(population, reference) * 10.0)
              << " pts from 500\n";
    std::cout << "headroom: " << static_cast<int>(bench::headroom(population) * 10.0)
              << " pts above the best model\n";
    std::cout << "\nsuggested difficulties (per scenario, by name):\n";
    const std::vector<int> d = bench::suggest_difficulties(population, reference);
    const auto& ref_reports = runs[reference].second;
    for (size_t i = 0; i < d.size() && i < ref_reports.size(); ++i)
        std::cout << "- " << ref_reports[i].name << ": " << d[i] << "\n";
    return 0;
}
// Dispatch the subcommand. Returns its exit code.
int dispatch_command(const std::string& cmd, const char* argv0, CliOptions& o) {
    if (cmd == "list")
        return cmd_list(o.suite);
    if (cmd == "run") {
        if (!o.rest.empty())
            o.name = o.rest[0];
        return cmd_run(o);
    }
    if (cmd == "validate-template" && !o.rest.empty())
        return cmd_validate(o.rest[0]);
    if (cmd == "report")
        return cmd_report(o.rest, o.format);
    if (cmd == "delta")
        return cmd_delta(o.rest);
    if (cmd == "scorecard" && !o.rest.empty()) {
        bench::RunMeta meta;
        std::vector<bench::ScenarioReport> reports;
        if (!parse_report_file(o.rest[0], meta, reports))
            return 1;
        std::cout << bench::render_scorecard(reports, meta);
        return 0;
    }
    if (cmd == "calibrate" && !o.rest.empty())
        return cmd_calibrate(o.rest);
    print_usage(argv0);
    return 1;
}

} // namespace

int main(int argc, char** argv) try {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    const std::string cmd = argv[1];
    CliOptions o;
    if (std::optional<int> rc = parse_cli(argc, argv, o))
        return *rc;
    try {
        return dispatch_command(cmd, argv[0], o);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
} catch (const std::exception& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
}
