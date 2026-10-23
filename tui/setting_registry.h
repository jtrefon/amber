
#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace tui {

// The behaviour bound to one dotted key. Existence, help, choices and ranges
// are NOT here: completions.json owns every piece of metadata, and code only
// says how to read and write the value. Adding a second metadata source here
// is the "code custom work" the command-tree contract forbids.
struct Setting {
    std::string key;                                // dotted path, e.g. "detection.loop"
    std::function<std::string()> getter;            // returns current value as string
    std::function<void(const std::string&)> setter; // sets from string
};

// Registry of all settings, indexed by dotted key path.
// Provides lookup, completion, namespace grouping, and validation.
class SettingRegistry {
public:
    // The fields a completions.json node may carry.
    struct NodeFields {
        std::string help;
        std::string man;
        std::string action;
        std::vector<std::string> aliases;
        std::string short_name;
        std::vector<std::string> choices;
        double range_lo = 0;
        double range_hi = 0;
        bool has_range = false;
    };

    // A resolved slash-command path. `consumed` counts the original tokens
    // belonging to the command; the rest are the handler argument.
    struct CommandMatch {
        const nlohmann::json* node = nullptr;
        std::size_t consumed = 0;
        bool ambiguous = false;
        std::vector<std::string> candidates;
    };

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
    // names). Used to inject dynamic namespaces (plugins, feeds) at runtime.
    //
    // Documented fields are immutable: a merge adds children but never rewrites
    // a field that completions.json supplied. Fields on dynamically introduced
    // nodes may be refreshed by their feed (provider state, job state, ...).
    // A plugin contributes a JSON subtree like any other integration; it may
    // extend a documented namespace (the MCP plugin's /get mcp) but cannot
    // overwrite an action/help/man or name a core action. Every attempted
    // write to a documented field is counted in merge_collisions().
    bool merge_completions_json(const nlohmann::json& subtree);

    // Attempts a merge made to overwrite a documented node. The documented
    // value always wins; this is the collision report.
    std::size_t merge_collisions() const { return merge_collisions_; }

    // Refused alias-layer writes: a visible alias key shadowing a documented
    // root, a key dual-booked with the legacy map, or a feed/plugin subtree
    // attempting to write the maps. The trees stay canonical either way.
    std::size_t alias_collisions() const;

    // Canonical display path behind a visible alias spelling ("model" →
    // "set.model"), empty for every non-alias key. The drawer hints the
    // mapping on the alias row.
    std::string alias_target_path(const std::string& key) const;

    // Clear all JSON-derived metadata (help/man/choices/children). Used to
    // rebuild the index after plugin namespaces are added or removed.
    void reset_completion_index();

    // The merged command tree (top-level keys are command names), used by the
    // slash-command dispatcher to resolve actions from input tokens.
    const nlohmann::json& command_tree() const { return tree_; }

    // Resolve a slash-command path: exact name, exact alias, exact short,
    // glued short path, then a unique prefix of any of those. Ambiguity is
    // reported, never resolved to a first match. `legacy` maps removed action
    // roots to canonical paths without reintroducing those roots.
    CommandMatch resolve_command(const std::vector<std::string>& tokens) const;

    // The documented short for a path, or empty when it has none. Used by the
    // drawer/help; dispatch derives a first-letter short when this is empty.
    std::string short_for(const std::string& path) const;

    // Sibling nodes that declare the same short. Zero for the shipped tree.
    std::size_t shortcut_collisions() const { return shortcut_collisions_; }

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
    const nlohmann::json* find_namespace_node(const std::string& prefix) const;
    void index_entry(const std::string& key, const NodeFields& f);
    void index_children(const std::string& display_path, const std::string& key_path,
                        const nlohmann::json& node);
    void index_node(const nlohmann::json& node, const std::string& display_path);
    void clear_index();
    void reindex();
    void rebuild_shortcut_collisions();
    void record_static_paths(const nlohmann::json& node, const std::string& path);
    void merge_tree_node(nlohmann::json& dst, const nlohmann::json& src, const std::string& path);
    void merge_children(nlohmann::json& dst, const nlohmann::json& children,
                        const std::string& path);
    void merge_field(nlohmann::json& dst, const std::string& key, const nlohmann::json& value);

    // Canonical full-path key for a possibly-dotted key: exact match, then
    // the "get.<key>" / "set.<key>" namespace probes. Empty when not indexed.
    std::string resolve_key(const std::string& key) const;

    nlohmann::json tree_;

    // Every node path completions.json supplied. A path in here is documented
    // and immutable; a path absent from it was introduced dynamically.
    std::set<std::string> static_paths_;
    std::size_t merge_collisions_ = 0;

    std::vector<Setting> settings_;
    std::map<std::string, std::string> key_help_;
    std::map<std::string, std::string> key_man_;
    std::map<std::string, std::vector<std::string>> key_children_;

    // Per-command completion lists (from JSON).
    std::map<std::string, std::vector<std::string>> command_choices_;
    std::map<std::string, std::vector<std::string>> command_aliases_;
    std::map<std::string, std::string> command_shorts_;
    std::map<std::string, std::vector<std::string>> legacy_paths_;
    std::map<std::string, std::pair<double, double>> command_ranges_;
    std::map<std::string, std::vector<std::string>> command_subcommands_;
    std::size_t shortcut_collisions_ = 0;
};

} // namespace tui
