
#ifndef BENCH_TEMPLATE_H
#define BENCH_TEMPLATE_H

// Static-template engine: scores an agent's artifact (coding/refactor
// scenarios) objectively by compiling hidden tests against it, running them,
// applying structural pattern checks, and comparing behavior to the reference
// solution. See docs/spec/benchmark/kpi-framework.md (template engine).

#include <string>
#include <vector>

namespace bench {

struct StructureCheck {
    std::string kind;     // "must_contain" | "must_not_contain"
    std::string pattern;
};

struct TemplateResult {
    bool compile_ok = false;
    int tests_passed = 0;
    int tests_total = 0;
    double structure_checks = 0.0;   // passed / total
    bool behavior_equivalent = false;
    long artifact_loc = 0;
    int duplicate_blocks = 0;        // -1 = detector unavailable
    int static_findings = 0;         // -1 = analyzer unavailable
};

// Load checks.json from the template dir. Returns false on parse error.
bool load_structure_checks(const std::string& template_dir,
                           std::vector<StructureCheck>& out, std::string& err);

// Score the agent's artifact (files under artifact_dir) against the template
// under template_dir. `err` carries a human-readable failure description.
TemplateResult run_template(const std::string& template_dir,
                            const std::string& artifact_dir,
                            const std::string& compiler, std::string& err);

} // namespace bench

#endif // BENCH_TEMPLATE_H
