
#include "bench/template.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>

#include "agent/tool.h"

namespace fs = std::filesystem;

namespace bench {

namespace {

const char* kChecksFile = "checks.json";

std::string read_all(const fs::path& p) {
    std::ifstream f(p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::vector<fs::path> source_files(const fs::path& dir, const char* ext) {
    std::vector<fs::path> out;
    if (!fs::is_directory(dir)) return out;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.is_regular_file() && e.path().extension() == ext)
            out.push_back(e.path());
    }
    return out;
}

bool file_has_checks(const fs::path& p) noexcept {
    return fs::is_regular_file(p) &&
           (p.extension() == ".cpp" || p.extension() == ".h");
}

std::string artifact_text(const fs::path& artifact_dir) {
    std::string all;
    if (!fs::is_directory(artifact_dir)) return all;
    for (const auto& e : fs::recursive_directory_iterator(artifact_dir)) {
        if (e.is_regular_file() && file_has_checks(e.path()))
            all += read_all(e.path()) + "\n";
    }
    return all;
}

long artifact_loc(const fs::path& artifact_dir) {
    long loc = 0;
    if (!fs::is_directory(artifact_dir)) return loc;
    for (const auto& e : fs::recursive_directory_iterator(artifact_dir)) {
        if (!e.is_regular_file() || !file_has_checks(e.path())) continue;
        const std::string text = read_all(e.path());
        loc += static_cast<long>(
            std::count(text.begin(), text.end(), '\n'));
    }
    return loc;
}

// Run a command capturing stdout. Returns exit status via the status out
// parameter (0 = success).
std::string run_capture(const std::string& cmd, int* status) {
    std::string out;
    FILE* f = popen(cmd.c_str(), "r");
    if (!f) {
        *status = -1;
        return out;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    *status = WEXITSTATUS(pclose(f));
    return out;
}

std::string compile_cmd(const std::string& compiler,
                        const std::vector<fs::path>& artifact_sources,
                        const fs::path& solution_dir, const fs::path& test_file,
                        const fs::path& bin) {
    std::ostringstream cmd;
    cmd << compiler << " -std=c++17 -w -I\"" << fs::absolute(solution_dir).string()
        << "\" ";
    for (const auto& s : artifact_sources) cmd << '"' << s.string() << "\" ";
    cmd << '"' << test_file.string() << "\" -o \"" << bin.string() << '"';
    return cmd.str();
}

struct TestOutcome {
    bool compiled = false;
    bool passed = false;
    std::string output;
};

TestOutcome run_one_test(const std::string& compiler,
                         const std::vector<fs::path>& artifact_sources,
                         const fs::path& solution_dir, const fs::path& test_file,
                         const fs::path& cache) {
    TestOutcome out;
    const fs::path bin = cache / (test_file.stem().string() + ".bin");
    if (std::system((compile_cmd(compiler, artifact_sources, solution_dir, test_file, bin) +
                     " 2>/dev/null")
                        .c_str()) != 0)
        return out;
    out.compiled = true;
    int status = -1;
    out.output = run_capture('"' + bin.string() + '"', &status);
    out.passed = status == 0;
    return out;
}

// The agent's source set is the skeleton's contract files only — stray files
// the agent left behind (own scratch tests) must not enter the build.
std::vector<fs::path> contract_sources(const fs::path& skeleton_dir,
                                       const fs::path& artifact_dir) {
    std::vector<fs::path> out;
    if (!fs::is_directory(skeleton_dir)) return out;
    for (const auto& e : fs::directory_iterator(skeleton_dir)) {
        if (!e.is_regular_file() || e.path().extension() != ".cpp") continue;
        const fs::path candidate = artifact_dir / e.path().filename();
        if (fs::is_regular_file(candidate)) out.push_back(candidate);
    }
    return out;
}

std::string duplicate_blocks(const fs::path& artifact_dir) {
    const fs::path detector = fs::current_path() / "tools" / "duplicate_detector.py";
    if (!fs::is_regular_file(detector)) return "";
    int status = 0;
    return run_capture("python3 \"" + detector.string() +
                           "\" --min-lines=6 \"" + artifact_dir.string() +
                           "\" 2>/dev/null",
                       &status);
}

} // namespace

bool load_structure_checks(const std::string& template_dir,
                           std::vector<StructureCheck>& out, std::string& err) {
    const fs::path p = fs::path(template_dir) / kChecksFile;
    if (!fs::is_regular_file(p)) {
        err = "template has no checks.json: " + p.string();
        return false;
    }
    agent::json j;
    try {
        j = agent::json::parse(read_all(p));
    } catch (const std::exception& e) {
        err = std::string("checks.json is not valid JSON: ") + e.what();
        return false;
    }
    if (!j.is_object()) {
        err = "checks.json must be an object";
        return false;
    }
    for (const char* kind : {"must_contain", "must_not_contain"}) {
        if (!j.contains(kind)) continue;
        if (!j[kind].is_array()) {
            err = std::string("checks.json: ") + kind + " must be an array";
            return false;
        }
        for (const auto& e : j[kind]) {
            if (!e.is_string()) {
                err = std::string("checks.json: ") + kind +
                      " entries must be strings";
                return false;
            }
            out.push_back({kind, e.get<std::string>()});
        }
    }
    return true;
}

TemplateResult run_template(const std::string& template_dir,
                            const std::string& artifact_dir,
                            const std::string& compiler, std::string& err) {
    TemplateResult r;
    const fs::path tpl(template_dir);
    const fs::path hidden = tpl / "hidden_tests";
    const fs::path reference = tpl / "reference";
    const fs::path artifact(artifact_dir);

    std::vector<StructureCheck> checks;
    if (!load_structure_checks(template_dir, checks, err)) return r;

    const std::vector<fs::path> tests = source_files(hidden, ".cpp");
    if (tests.empty()) {
        err = "template has no hidden tests: " + hidden.string();
        return r;
    }

    const std::string text = artifact_text(artifact);
    size_t passed_checks = 0;
    for (const auto& c : checks) {
        const bool present = text.find(c.pattern) != std::string::npos;
        if ((c.kind == "must_contain" && present) ||
            (c.kind == "must_not_contain" && !present))
            ++passed_checks;
    }
    r.structure_checks = checks.empty()
                             ? 1.0
                             : static_cast<double>(passed_checks) /
                                   static_cast<double>(checks.size());
    r.artifact_loc = artifact_loc(artifact);

    const std::string dup_out = duplicate_blocks(artifact);
    size_t pos = 0;
    size_t dup_count = 0;
    while ((pos = dup_out.find("<->", pos)) != std::string::npos) {
        ++dup_count;
        pos += 3;
    }
    r.duplicate_blocks = static_cast<int>(dup_count);

    const fs::path cache =
        fs::temp_directory_path() /
        ("amber_bench_tpl_" + std::to_string(static_cast<long>(::getpid())));
    fs::create_directories(cache);

    std::vector<TestOutcome> reference_outcomes;
    reference_outcomes.reserve(tests.size());
    const std::vector<fs::path> ref_sources =
        contract_sources(reference, reference);
    for (const auto& t : tests)
        reference_outcomes.push_back(
            run_one_test(compiler, ref_sources, reference, t, cache));

    r.tests_total = static_cast<int>(tests.size());
    r.compile_ok = true;
    bool reference_all_good = true;
    std::vector<fs::path> art_sources =
        contract_sources(tpl / "skeleton", artifact);
    if (art_sources.empty())
        art_sources = source_files(artifact, ".cpp");
    for (size_t i = 0; i < tests.size(); ++i) {
        TestOutcome ao =
            run_one_test(compiler, art_sources, artifact, tests[i], cache);
        if (!ao.compiled) {
            r.compile_ok = false;
            reference_all_good = false;
            continue;
        }
        if (ao.passed) ++r.tests_passed;
        const TestOutcome& ro = reference_outcomes[i];
        if (!ro.compiled || !ro.passed || !ao.passed ||
            ro.output != ao.output)
            reference_all_good = false;
    }
    r.behavior_equivalent = reference_all_good;
    fs::remove_all(cache);
    err.clear();
    return r;
}

} // namespace bench
