
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "agent.h"
#include "agent/version.h"
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
              << "                               multiple files render a model comparison\n\n"
              << "Options:\n"
              << "  --suite NAME     filter by suite\n"
              << "  --scenario NAME  run a single scenario\n"
              << "  --live           use the real model (amber.conf / env / flags)\n"
              << "  --profile NAME   model profile from bench/profiles.json\n"
              << "  --model NAME     model override (live)\n"
              << "  --temperature T  temperature override (live)\n"
              << "  --repeat N       run each scenario N times\n"
              << "  --out FILE       write JSON report to FILE\n"
              << "  --format FMT     report output: text (default), markdown, json\n"
              << "  -h, --help       show this help\n";
}

std::vector<bench::Scenario> discover_scenarios(const std::string& suite,
                                                const std::string& name) {
    std::vector<bench::Scenario> out;
    const fs::path root = fs::current_path() / "bench" / "scenarios";
    if (!fs::is_directory(root)) {
        std::cerr << "error: no bench/scenarios directory in " << fs::current_path().string() << "\n";
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
        if (!suite.empty() && s->suite != suite) continue;
        if (!name.empty() && s->name != name) continue;
        out.push_back(std::move(*s));
    }
    return out;
}

void apply_profile(bench::RunOptions& opts, const std::string& profile) {
    if (profile.empty()) return;
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
    if (p.contains("model") && p["model"].is_string()) opts.model = p["model"].get<std::string>();
    if (p.contains("temperature") && p["temperature"].is_number())
        opts.temperature = p["temperature"].get<double>();
    if (p.contains("thinking") && p["thinking"].is_string())
        opts.thinking = p["thinking"].get<std::string>();
    if (p.contains("thinking_budget") && p["thinking_budget"].is_number_integer())
        opts.thinking_budget = p["thinking_budget"].get<int>();
}

std::string run_id() {
    std::string id = "run-";
    id += std::to_string(static_cast<long long>(
        std::chrono::system_clock::now().time_since_epoch().count()));
    return id;
}

int cmd_run(const std::vector<std::string>& args, bool live,
            const std::string& suite, const std::string& name,
            const std::string& profile, const std::string& model,
            double temperature, int repeat, const std::string& out_file,
            const std::string& debug_dir) {
    (void)args;
    bench::RunOptions opts;
    opts.live = live;
    opts.repeat = repeat;
    opts.model = model;
    opts.temperature = temperature;
    opts.debug_dir = debug_dir;
    apply_profile(opts, profile);

    auto scenarios = discover_scenarios(suite, name);
    if (scenarios.empty()) {
        std::cerr << "error: no scenarios match\n";
        return 1;
    }

    bench::RunMeta meta;
    meta.run_id = run_id();
    meta.mode = live ? "live" : "hermetic";
    meta.profile = profile;
        meta.model = opts.model;
    if (meta.model.empty()) meta.model = live ? "config" : "fake";
    meta.engine_version = agent::kVersion;
    meta.timestamp = std::to_string(static_cast<long long>(
        std::chrono::system_clock::now().time_since_epoch().count()));
    meta.reasoning = opts.thinking.empty() ? "auto" : opts.thinking;

    std::vector<bench::ScenarioReport> reports =
        bench::run_scenarios(scenarios, opts, meta);
    std::cout << bench::render_text(reports, meta);

    if (!out_file.empty()) {
        std::ofstream out(out_file);
        out << bench::render_json(reports, meta);
    }
    return 0;
}

int cmd_list(const std::string& suite) {
    auto scenarios = discover_scenarios(suite, "");
    for (const auto& s : scenarios) {
        std::cout << s.suite << "/" << s.name
                  << (s.hermetic_only ? " [hermetic]" : "")
                  << (s.template_dir.empty() ? "" : " [template]") << "\n";
        if (!s.description.empty()) std::cout << "    " << s.description << "\n";
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
        bench::run_template(tpl.string(), (tpl / "reference").string(),
                            "g++", err);
    std::cout << "template " << s.name << ": "
              << r.tests_passed << "/" << r.tests_total << " hidden tests pass"
              << (r.compile_ok ? "" : " (compile failed)") << "\n";
    if (!err.empty()) std::cerr << "error: " << err << "\n";
    return (r.compile_ok && r.tests_passed == r.tests_total) ? 0 : 1;
}

bool parse_report_file(const std::string& file, bench::RunMeta& meta,
                       std::vector<bench::ScenarioReport>& reports) {
    std::ifstream f(file);
    if (!f) {
        std::cerr << "error: cannot open " << file << "\n";
        return false;
    }
    agent::json j;
    try {
        j = agent::json::parse(f);
    } catch (const std::exception& e) {
        std::cerr << "error: " << file << ": " << e.what() << "\n";
        return false;
    }
    if (!bench::parse_report_json(j, meta, reports)) {
        std::cerr << "error: " << file << " is not an amber-bench report\n";
        return false;
    }
    return true;
}

int cmd_report(const std::vector<std::string>& files,
               const std::string& format) {
    if (files.empty()) {
        std::cerr << "error: report needs at least one results file\n";
        return 1;
    }
    std::vector<std::pair<bench::RunMeta, std::vector<bench::ScenarioReport>>> runs;
    for (const auto& f : files) {
        bench::RunMeta meta;
        std::vector<bench::ScenarioReport> reports;
        if (!parse_report_file(f, meta, reports)) return 1;
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

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    const std::string cmd = argv[1];

    bool live = false;
    std::string suite, name, profile, model, out_file, format = "text";
    std::string opts_debug_dir;
    double temperature = -1;
    int repeat = 1;
    std::vector<std::string> rest;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* def) -> std::string {
            if (i + 1 < argc) return argv[++i];
            return def;
        };
        if (a == "--live") live = true;
        else if (a == "--suite") suite = next("");
        else if (a == "--scenario") name = next("");
        else if (a == "--profile") profile = next("");
        else if (a == "--model") model = next("");
        else if (a == "--temperature") temperature = std::atof(next("0").c_str());
        else if (a == "--repeat") repeat = std::atoi(next("1").c_str());
        else if (a == "--debug") opts_debug_dir = next("");
        else if (a == "--out") out_file = next("");
        else if (a == "--format") format = next("text");
        else if (a == "-h" || a == "--help") { print_usage(argv[0]); return 0; }
        else rest.push_back(a);
    }

    try {
        if (cmd == "list") return cmd_list(suite);
        if (cmd == "run") {
            if (!rest.empty()) name = rest[0];
            return cmd_run(rest, live, suite, name, profile, model, temperature,
                           repeat, out_file, opts_debug_dir);
        }
        if (cmd == "validate-template" && !rest.empty())
            return cmd_validate(rest[0]);
        if (cmd == "report") return cmd_report(rest, format);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    print_usage(argv[0]);
    return 1;
}
