
#ifndef BENCH_SCENARIO_H
#define BENCH_SCENARIO_H

// Declarative benchmark scenario: setup, prompt, tool-call oracle, answer
// checks, budgets. See docs/spec/benchmark/kpi-framework.md for the schema.

#include <map>
#include <string>
#include <vector>
#include <optional>

#include "agent/tool.h"

namespace bench {

struct ScenarioStep {
    std::string tool;
    agent::json args;        // expected args; null -> match any
    bool args_subset = true; // default: match when every expected key is
                             // present (calls may add optional args like
                             // read's limit or write's edits). Set false
                             // to require an exact key set.
    bool unordered = false;  // may match any remaining position in the step set
};

struct TemplateSpec {
    std::string dir;          // relative path under bench/scenarios/
    std::string compile_with; // e.g. "g++"
};

struct Checks {
    std::vector<std::string> must_contain;
    // Any-of groups: each inner list passes when ANY member matches; the
    // group counts as one check. Lets review scenarios accept legitimate
    // alternative phrasings (hash map vs unordered_set, strategy vs visitor).
    std::vector<std::vector<std::string>> must_contain_any;
    std::vector<std::string> must_not_contain;
};

// One user turn in a multi-turn scenario. The turn's script and oracle are
// appended to the scenario's effective script and oracle at load time, so the
// existing runner and scoring see one continuous stream in turn order.
struct Turn {
    std::string prompt;
    agent::json fake_replies = agent::json::array();
    std::vector<ScenarioStep> oracle;
    Checks checks;
};

struct Scenario {
    std::string name;
    std::string suite;
    std::string description;
    // CLI-tier provider selection: a dialect scenario names the provider preset
    // and flavor its requests must use. Empty leaves the host configuration.
    std::string provider;
    std::string flavor;
    // External project plugins the scenario relies on. A freshly discovered v1
    // plugin is off by default; a headless host enables exactly these.
    std::vector<std::string> enable_plugins;
    std::vector<std::string> platforms; // "linux" | "darwin"; empty = all
    bool hermetic_only = false;
    std::vector<std::string> model_profiles;
    agent::json setup = agent::json::object(); // {files: {path: content}, shell: [...]}
    std::string prompt;
    std::vector<Turn> turns;                         // multi-turn script, empty = one shot
    agent::json fake_replies = agent::json::array(); // hermetic script
    agent::json policy = agent::json::object();      // closed-loop state machine; empty = FIFO
    agent::json expected_workspace = agent::json::object();
    // A workspace assertion the CLI tier makes after a behaviour the in-process
    // reference run cannot reproduce (a cancelled tool leaves no artifact).
    agent::json cli_expected_workspace = agent::json::object();
    agent::json request_contract = agent::json::object();
    std::vector<agent::json> faults;
    std::vector<agent::json> subagent_replies; // hermetic: per-sub-agent scripts
    bool stream = false;                       // hermetic: use the streaming path
    // Hermetic scripts that need a side-effecting tool declare "approval":
    // "allow". The default keeps the benchmark's deny-unless-process policy so
    // the denied-tool scenarios still exercise the refusal path.
    bool allow_tools = false;
    // Declared environment axes. `--matrix` runs the cross-product (capped at
    // 8 cells); `--variant key=value` runs one cell. Empty = no variants.
    std::map<std::string, std::vector<agent::json>> variants;
    int context_size = 0; // 0 = the hermetic default
    std::vector<ScenarioStep> oracle;
    std::vector<std::string> forbidden_tools;
    Checks prompt_checks;
    Checks checks;
    agent::json optimal_plan = agent::json::object(); // tool -> count; empty = oracle mix
    std::string template_dir;                         // empty = no static template
    // Weight of the textual checks in the correctness sub-score (0..1).
    // Review scenarios (code-snippet issue lists) set 0.8 so the checks —
    // recall of expected issues, precision against distractors — dominate
    // over the single read-step bullseye. Default 0.2 reproduces the v2
    // non-template formula.
    double checks_weight = 0.2;
    int difficulty = 3;      // 1..5 — scoring weight
    int expected_steps = 0;  // 0 = oracle size (or 5) — efficiency baseline
    int max_steps = 0;       // 0 = unlimited
    long max_wall_ms = 0;    // 0 = unlimited
    int min_compactions = 0; // 0 = unchecked
    // 0 = unchecked. When set, the run must have compacted and the compaction's
    // last output/input token ratio must be at most this (1.0 = no growth).
    double max_compaction_ratio = 0.0;
    bool detection_loop = false;      // enable tool/text-loop detection
    bool detection_duplicate = false; // enable duplicate-call detection
    // Binary tier expectations. `expected_exit_code` is the process exit the
    // CLI must return; the token floors catch a run that answered without
    // talking to the provider (or a usage mapping that silently dropped to 0).
    int expected_exit_code = 0;
    long min_prompt_tokens = 0;     // 0 = unchecked
    long min_completion_tokens = 0; // 0 = unchecked
    // Binary tier session support: --session NAME persists the conversation,
    // and `resume` runs each turn as a separate CLI process against it.
    std::string session;
    bool resume = false;
    // Send SIGINT to the CLI when this text appears on its stderr; the journey
    // asserts the normal cancel path (exit 130, no partial artifact).
    std::string cancel_marker;
    // Strings that must appear somewhere in the endpoint's chat request bodies
    // (context carry, a worker report, a skill body).
    std::vector<std::string> request_log_contains;
    // The CLI endpoint's FIFO when it differs from the in-process script: a
    // subagent run sees the parent's and the worker's replies on one wire, and
    // a compression pass appends its classify/extract replies here.
    agent::json endpoint_replies = agent::json::array();
    // The CLI tier's full tool sequence when it includes calls the in-process
    // parent does not record (a subagent's worker tools). Empty = the
    // in-process run's sequence.
    std::vector<std::string> expected_tools;
    // Compression overrides for the CLI tier and the in-process reference run.
    // <0 leaves the Config default; the journey sets a small window and a low
    // threshold so one turn crosses the gate.
    double compression_threshold = -1.0;
    int compression_min_turns = -1;
    int compression_cooldown_turns = -1;
    int compression_target_pct = -1;
};

// Parse and validate a scenario file. Returns nullopt with a message on error.
std::optional<Scenario> load_scenario(const std::string& path, std::string& err);

// Lay a scenario's `setup.files` down under `workspace`. `@ROOT@` in a file
// body (or an `executables` entry) expands to the checkout root, and every
// `setup.executables` path becomes owner/group/other executable. Both the
// in-process runner and the CLI driver use this, so a scenario's MCP server or
// external plugin behaves the same in each tier.
void materialize_setup_files(const std::string& workspace, const agent::json& setup);

// Expand the `@ROOT@` placeholder in a setup shell command or test assertion.
std::string expand_setup_text(std::string text, const std::string& root);

// Whether the scenario can run on the current host (platforms gate).
bool platform_supported(const Scenario& s);

// Expand a scenario's declared variants into runnable cells. `matrix` runs the
// cross-product (capped at 8); `variant` selects one `key=value` cell. No
// declared variants returns the scenario unchanged.
std::vector<Scenario> expand_variants(const Scenario& s, bool matrix, const std::string& variant,
                                      std::string& err);

// Whether all textual checks hold for the given text (must/must-not contain).
bool checks_pass(const Checks& c, const std::string& text) noexcept;

// Fraction of checks satisfied by the text (adherence metric).
double adherence(const Checks& c, const std::string& text) noexcept;

} // namespace bench

#endif // BENCH_SCENARIO_H
