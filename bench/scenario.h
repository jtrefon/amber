
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
    bool args_subset = true;    // default: match when every expected key is
                                // present (calls may add optional args like
                                // read's limit or write's edits). Set false
                                // to require an exact key set.
    bool unordered = false;     // may match any remaining position in the step set
};

struct TemplateSpec {
    std::string dir;            // relative path under bench/scenarios/
    std::string compile_with;   // e.g. "g++"
};

struct Checks {
    std::vector<std::string> must_contain;
    // Any-of groups: each inner list passes when ANY member matches; the
    // group counts as one check. Lets review scenarios accept legitimate
    // alternative phrasings (hash map vs unordered_set, strategy vs visitor).
    std::vector<std::vector<std::string>> must_contain_any;
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
    std::vector<agent::json> subagent_replies;        // hermetic: per-sub-agent scripts
    bool stream = false;        // hermetic: use the streaming path
    std::vector<ScenarioStep> oracle;
    std::vector<std::string> forbidden_tools;
    Checks prompt_checks;
    Checks checks;
    agent::json optimal_plan = agent::json::object();  // tool -> count; empty = oracle mix
    std::string template_dir;   // empty = no static template
    // Weight of the textual checks in the correctness sub-score (0..1).
    // Review scenarios (code-snippet issue lists) set 0.8 so the checks —
    // recall of expected issues, precision against distractors — dominate
    // over the single read-step bullseye. Default 0.2 reproduces the v2
    // non-template formula.
    double checks_weight = 0.2;
    int difficulty = 3;         // 1..5 — scoring weight
    int expected_steps = 0;     // 0 = oracle size (or 5) — efficiency baseline
    int max_steps = 0;          // 0 = unlimited
    long max_wall_ms = 0;       // 0 = unlimited
    bool task_tool = false;     // enable the task tool for this scenario
    bool detection_loop = false;     // enable tool/text-loop detection
    bool detection_duplicate = false;  // enable duplicate-call detection
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
