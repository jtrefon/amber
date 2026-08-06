
#pragma once

#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

namespace tui {

// A single setting (config key) with its metadata and accessors.
// Both /get and /set use this as their single source of truth for
// completion, help text, validation, and dispatch.
struct Setting {
    std::string key;               // dotted path, e.g. "detection.loop"
    std::string help;              // human-readable description
    std::string placeholder;       // e.g. "<on|off|toggle>"

    enum Type { Choice, Int, Float, Bool, String } type = Choice;
    std::vector<std::string> choices;   // for Choice type
    double range_min = 0, range_max = 0; // for Int/Float

    std::function<std::string()> getter;     // returns current value as string
    std::function<void(const std::string&)> setter;  // sets from string
};

// Registry of all settings, indexed by dotted key path.
// Provides lookup, completion, namespace grouping, and validation.
class SettingRegistry {
public:
    void add(Setting s);
    const Setting* find(const std::string& key) const;

    // All top-level namespaces (e.g. "detection", "compression").
    std::vector<std::string> namespaces() const;

    // All keys under a namespace (e.g. "loop", "duplicate" for "detection").
    std::vector<std::string> keys_in(const std::string& ns) const;

    // Full dotted keys matching a prefix.
    std::vector<std::string> complete(const std::string& prefix) const;

    // All full dotted keys.
    const std::vector<Setting>& all() const { return settings_; }

    // Load completion metadata from a JSON file. The file is the source of truth
    // for: which keys exist per command, their help text, choices, and ranges.
    // Code-based getter/setter lambdas (from build_settings()) are preserved —
    // this only adds/overlays metadata.
    bool load_completions_json(const std::string& path);

    // Merge a completions.json-shaped subtree (top-level keys are command
    // names). Used to inject plugin namespaces at runtime.
    bool merge_completions_json(const nlohmann::json& subtree);

    // Clear all JSON-derived metadata (help/man/choices/children). Used to
    // rebuild the index after plugin namespaces are added or removed.
    void reset_completion_index();

    // The merged command tree (top-level keys are command names), used by the
    // slash-command dispatcher to resolve actions from input tokens.
    const nlohmann::json& command_tree() const { return tree_; }

    // Get help text for a setting key (from JSON). Dotted keys resolve to the
    // canonical full path: exact match, then "get.<key>", then "set.<key>"
    // (e.g. "policy.mode" → "set.policy.mode").
    std::string help_for(const std::string& key) const;

    // Get full manual text for a setting key (from JSON "man" field).
    std::string man_for(const std::string& key) const;

    // Child keys of a namespace node loaded from JSON "children" keys.
    // Namespaces are indexed by their FULL display path ("set.model" and
    // "get.model" are distinct); dotted keys resolve as in help_for().
    // e.g. children_of("set.policy") → ["approval", "mode", "timeout"]
    std::vector<std::string> children_of(const std::string& key) const;

    // Aliases declared in the JSON ("aliases": ["w"]) for top-level
    // commands, so the no-space completion branch stays tree-driven.
    std::vector<std::string> top_level_aliases() const;
    const std::vector<std::string>& aliases_for(const std::string& key) const;

    // Access choices and ranges loaded from JSON (for help display).
    const std::vector<std::string>& choices_for(const std::string& key) const;
    bool range_for(const std::string& key, double& lo, double& hi) const;

    // Subcommand lists indexed by command name (from JSON "children" keys).
    // e.g. "system" → ["exec", "delete", "rmdir", ...]
    const std::vector<std::string>& subcommands_for(const std::string& cmd) const;

private:
    void index_node(const nlohmann::json& node, const std::string& display_path);

    // Canonical full-path key for a possibly-dotted key: exact match, then
    // the "get.<key>" / "set.<key>" namespace probes. Empty when not indexed.
    std::string resolve_key(const std::string& key) const;

    nlohmann::json tree_;

    std::vector<Setting> settings_;
    std::map<std::string, std::string> key_help_;
    std::map<std::string, std::string> key_man_;
    std::map<std::string, std::vector<std::string>> key_children_;

    // Per-command completion lists (from JSON).
    std::map<std::string, std::vector<std::string>> command_choices_;
    std::map<std::string, std::vector<std::string>> command_aliases_;
    std::map<std::string, std::pair<double,double>> command_ranges_;
    std::map<std::string, std::vector<std::string>> command_subcommands_;
};

} // namespace tui
