
#ifndef BENCH_SCENARIO_H
#define BENCH_SCENARIO_H

// Declarative benchmark scenario: setup, prompt, tool-call oracle, answer
// checks, budgets. See docs/spec/benchmark/kpi-framework.md for the schema.

#include <string>
#include <vector>
#include <optional>

#include "agent/tool.h"

namespace bench {

struct ScenarioStep {
    std::string tool;
    agent::json args;           // expected args; null -> match any
    bool args_subset = false;   // match when every expected key is present
    bool unordered = false;     // may match any remaining position in the step set
};

struct TemplateSpec {
    std::string dir;            // relative path under bench/scenarios/
    std::string compile_with;   // e.g. "g++"
};

struct Checks {
    std::vector<std::string> must_contain;
    std::vector<std::string> must_not_contain;
};

struct Scenario {
    std::string name;
    std::string suite;
    std::string description;
    std::vector<std::string> platforms;   // "linux" | "darwin"; empty = all
    bool hermetic_only = false;
    std::vector<std::string> model_profiles;
    agent::json setup = agent::json::object();  // {files: {path: content}, shell: [...]}
    std::string prompt;
    agent::json fake_replies = agent::json::array();  // hermetic script
    bool stream = false;        // hermetic: use the streaming path
    std::vector<ScenarioStep> oracle;
    std::vector<std::string> forbidden_tools;
    Checks prompt_checks;
    Checks checks;
    agent::json optimal_plan = agent::json::object();  // tool -> count; empty = oracle mix
    std::string template_dir;   // empty = no static template
    int difficulty = 3;         // 1..5 — scoring weight
    int expected_steps = 0;     // 0 = oracle size (or 5) — efficiency baseline
    int max_steps = 0;          // 0 = unlimited
    long max_wall_ms = 0;       // 0 = unlimited
};

// Parse and validate a scenario file. Returns nullopt with a message on error.
std::optional<Scenario> load_scenario(const std::string& path, std::string& err);

// Whether the scenario can run on the current host (platforms gate).
bool platform_supported(const Scenario& s) noexcept;

// Whether all textual checks hold for the given text (must/must-not contain).
bool checks_pass(const Checks& c, const std::string& text) noexcept;

// Fraction of checks satisfied by the text (adherence metric).
double adherence(const Checks& c, const std::string& text) noexcept;

} // namespace bench

#endif // BENCH_SCENARIO_H
