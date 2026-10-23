
#include "setting_registry.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace tui {

void SettingRegistry::add(Setting s) {
    settings_.push_back(std::move(s));
}

const Setting* SettingRegistry::find(const std::string& key) const {
    for (const auto& s : settings_)
        if (s.key == key)
            return &s;
    return nullptr;
}

std::vector<std::string> SettingRegistry::namespaces() const {
    std::vector<std::string> out;
    for (const auto& s : settings_) {
        size_t dot = s.key.find('.');
        std::string ns = (dot == std::string::npos) ? s.key : s.key.substr(0, dot);
        if (std::find(out.begin(), out.end(), ns) == out.end())
            out.push_back(ns);
    }
    return out;
}

std::vector<std::string> SettingRegistry::keys_in(const std::string& ns) const {
    std::vector<std::string> out;
    std::string prefix = ns + ".";
    for (const auto& s : settings_) {
        if (s.key.rfind(prefix, 0) == 0) {
            std::string tail = s.key.substr(prefix.size());
            if (std::find(out.begin(), out.end(), tail) == out.end())
                out.push_back(tail);
        }
    }
    return out;
}

// Walk to the namespace node: the first token is a top-level command, every
// following token descends through a "children" map. Returns nullptr when the
// path is not in the tree.
const nlohmann::json* SettingRegistry::find_namespace_node(const std::string& prefix) const {
    if (!tree_.contains("commands") || !tree_["commands"].is_object())
        return nullptr;
    const nlohmann::json* node = &tree_["commands"];
    size_t p = 0;
    while (p < prefix.size()) {
        const size_t dot = prefix.find('.', p);
        const std::string tok =
            (dot == std::string::npos) ? prefix.substr(p) : prefix.substr(p, dot - p);
        if (p == 0) {
            if (!node->contains(tok))
                return nullptr;
            node = &(*node)[tok];
        } else {
            if (!node->is_object() || !node->contains("children") ||
                !(*node)["children"].is_object() || !(*node)["children"].contains(tok))
                return nullptr;
            node = &(*node)["children"][tok];
        }
        if (dot == std::string::npos)
            break;
        p = dot + 1;
    }
    return node;
}

std::vector<std::string> SettingRegistry::complete(const std::string& prefix) const {
    std::vector<std::string> out;
    // The command tree is the authoritative structure. Query semantics:
    //   complete("")            → top-level keys
    //   complete("set")         → direct children paths of "set"
    //   complete("set.policy")  → direct children paths of "set.policy"
    // Direct children only — the drawer rows and the completion list must
    // stay 1:1 aligned for Enter dispatch.
    if (!tree_.contains("commands") || !tree_["commands"].is_object())
        return out;
    const nlohmann::json* node = prefix.empty() ? &tree_["commands"] : find_namespace_node(prefix);
    if (!node)
        return out;
    const nlohmann::json* kids = node;
    if (!prefix.empty()) {
        if (!node->contains("children") || !(*node)["children"].is_object())
            return out;
        kids = &(*node)["children"];
    }
    for (auto it = kids->begin(); it != kids->end(); ++it) {
        const std::string child = prefix.empty() ? it.key() : prefix + "." + it.key();
        if (std::find(out.begin(), out.end(), child) == out.end())
            out.push_back(child);
    }
    return out;
}

std::string SettingRegistry::resolve_key(const std::string& key) const {
    auto indexed = [&](const std::string& k) {
        return key_help_.count(k) || key_man_.count(k) || key_children_.count(k) ||
               command_choices_.count(k) || command_shorts_.count(k) || command_ranges_.count(k);
    };
    if (indexed(key))
        return key;
    std::string g = "get." + key;
    if (indexed(g))
        return g;
    std::string s = "set." + key;
    if (indexed(s))
        return s;
    return "";
}

std::string SettingRegistry::help_for(const std::string& key) const {
    std::string rk = resolve_key(key);
    if (rk.empty() || !key_help_.count(rk))
        return "";
    return key_help_.at(rk);
}

std::string SettingRegistry::man_for(const std::string& key) const {
    std::string rk = resolve_key(key);
    if (rk.empty() || !key_man_.count(rk))
        return "";
    return key_man_.at(rk);
}

std::vector<std::string> SettingRegistry::children_of(const std::string& key) const {
    std::string rk = resolve_key(key);
    auto it = key_children_.find(rk);
    if (it != key_children_.end())
        return it->second;
    // Bare "/" (empty key after stripping the slash) — top-level command
    // names from the JSON tree, mirroring complete(""). Without this the
    // bare "/" drawer path returns empty (key_children_ is never indexed
    // by ""). Only trigger when the caller's key was actually empty —
    // not when resolve_key failed to find a non-empty key.
    if (key.empty() && tree_.contains("commands") && tree_["commands"].is_object()) {
        std::vector<std::string> out;
        for (auto it2 = tree_["commands"].begin(); it2 != tree_["commands"].end(); ++it2)
            out.push_back(it2.key());
        return out;
    }
    return {};
}

// The string elements of an array field, empty when the field is absent.
std::vector<std::string> string_array(const nlohmann::json& node, const char* key) {
    std::vector<std::string> out;
    if (!node.contains(key) || !node[key].is_array())
        return out;
    for (const auto& v : node[key])
        out.push_back(v.get<std::string>());
    return out;
}

SettingRegistry::NodeFields read_node_fields(const nlohmann::json& node) {
    SettingRegistry::NodeFields f;
    if (node.contains("help") && node["help"].is_string())
        f.help = node["help"].get<std::string>();
    if (node.contains("man") && node["man"].is_string())
        f.man = node["man"].get<std::string>();
    if (node.contains("action") && node["action"].is_string())
        f.action = node["action"].get<std::string>();
    f.aliases = string_array(node, "aliases");
    if (node.contains("short") && node["short"].is_string())
        f.short_name = node["short"].get<std::string>();
    f.choices = string_array(node, "choices");
    if (node.contains("range") && node["range"].is_array() && node["range"].size() == 2) {
        f.range_lo = node["range"][0].get<double>();
        f.range_hi = node["range"][1].get<double>();
        f.has_range = true;
    }
    return f;
}

// The child keys of a node, or empty when it has no children object.
std::vector<std::string> child_keys(const nlohmann::json& node) {
    std::vector<std::string> keys;
    if (!node.contains("children") || !node["children"].is_object())
        return keys;
    for (auto it = node["children"].begin(); it != node["children"].end(); ++it)
        keys.push_back(it.key());
    return keys;
}

void union_into(std::vector<std::string>& dst, const std::vector<std::string>& src) {
    for (const auto& k : src)
        if (std::find(dst.begin(), dst.end(), k) == dst.end())
            dst.push_back(k);
}

void SettingRegistry::index_entry(const std::string& key, const NodeFields& f) {
    if (!f.help.empty())
        key_help_[key] = f.help;
    if (!f.man.empty())
        key_man_[key] = f.man;
    if (!f.choices.empty())
        command_choices_[key] = f.choices;
    if (f.has_range)
        command_ranges_[key] = {f.range_lo, f.range_hi};
}

// Index a namespace's children so complete() can walk the tree.
void SettingRegistry::index_children(const std::string& display_path, const std::string& key_path,
                                     const nlohmann::json& node) {
    const std::vector<std::string> kids = child_keys(node);
    if (kids.empty())
        return;
    if (!display_path.empty() && display_path.find('.') == std::string::npos)
        union_into(command_subcommands_[display_path], kids);
    if (!key_path.empty())
        union_into(key_children_[key_path], kids);
}

void SettingRegistry::index_node(const nlohmann::json& node, const std::string& display_path) {
    if (!node.is_object())
        return;
    const NodeFields f = read_node_fields(node);

    // Namespaces are indexed by their FULL display path so get.<ns> and
    // set.<ns> never collide ("set.model" != "get.model"). Top-level
    // commands keep their bare name ("model", "policy").
    const std::string& key_path = display_path;
    const bool nested = key_path.find('.') != std::string::npos;
    if (!f.action.empty())
        index_entry(f.action, f);
    if (!key_path.empty() && (nested || !node.contains("children") || !f.help.empty())) {
        index_entry(key_path, f);
        if (!f.aliases.empty())
            command_aliases_[key_path] = f.aliases;
    }
    if (!key_path.empty() && !f.short_name.empty())
        command_shorts_[key_path] = f.short_name;
    index_children(display_path, key_path, node);

    if (!node.contains("children") || !node["children"].is_object())
        return;
    for (auto it = node["children"].begin(); it != node["children"].end(); ++it) {
        const std::string child_display =
            display_path.empty() ? it.key() : display_path + "." + it.key();
        index_node(it.value(), child_display);
    }
}

bool SettingRegistry::load_completions_json(const std::string& path) {
    std::ifstream f(path);
    if (!f)
        return false;

    using json = nlohmann::json;
    json root;
    try {
        f >> root;
    } catch (...) {
        return false;
    }

    if (root.contains("commands") && root["commands"].is_object()) {
        tree_["commands"] = root["commands"];
        static_paths_.clear();
        legacy_paths_.clear();
        merge_collisions_ = 0;
        for (auto it = root["commands"].begin(); it != root["commands"].end(); ++it)
            record_static_paths(it.value(), it.key());
        if (root.contains("legacy") && root["legacy"].is_object()) {
            for (auto it = root["legacy"].begin(); it != root["legacy"].end(); ++it) {
                if (!it.value().is_string())
                    continue;
                std::istringstream in(it.value().get<std::string>());
                std::vector<std::string> tokens;
                std::string tok;
                while (in >> tok)
                    tokens.push_back(tok);
                if (!tokens.empty())
                    legacy_paths_[it.key()] = std::move(tokens);
            }
        }
        reindex();
    }
    return true;
}

void SettingRegistry::record_static_paths(const nlohmann::json& node, const std::string& path) {
    if (!node.is_object())
        return;
    static_paths_.insert(path);
    if (!node.contains("children") || !node["children"].is_object())
        return;
    for (auto it = node["children"].begin(); it != node["children"].end(); ++it) {
        const std::string child = path.empty() ? it.key() : path + "." + it.key();
        record_static_paths(it.value(), child);
    }
}

void SettingRegistry::merge_children(nlohmann::json& dst, const nlohmann::json& children,
                                     const std::string& path) {
    if (!children.is_object())
        return;
    if (!dst.contains("children") || !dst["children"].is_object())
        dst["children"] = nlohmann::json::object();
    for (auto cit = children.begin(); cit != children.end(); ++cit) {
        const std::string child = path.empty() ? cit.key() : path + "." + cit.key();
        merge_tree_node(dst["children"][cit.key()], cit.value(), child);
    }
}

void SettingRegistry::merge_field(nlohmann::json& dst, const std::string& key,
                                  const nlohmann::json& value) {
    if (!dst.contains(key) || dst[key] != value)
        ++merge_collisions_;
}

// Documented fields are immutable. A merge adds children (a documented
// command may also be a namespace: /set model is both the switch and the list
// of model rows) and fills in new dynamic branches, but never rewrites a
// field completions.json supplied -- a plugin or feed that tries is recorded
// as a collision and the documented value stays.
void SettingRegistry::merge_tree_node(nlohmann::json& dst, const nlohmann::json& src,
                                      const std::string& path) {
    const bool documented = static_paths_.count(path) > 0;
    if (!dst.is_object()) {
        if (documented)
            ++merge_collisions_;
        else
            dst = src;
        return;
    }
    for (auto it = src.begin(); it != src.end(); ++it) {
        if (it.key() == "children")
            merge_children(dst, it.value(), path);
        else if (documented)
            merge_field(dst, it.key(), it.value());
        else
            dst[it.key()] = it.value();
    }
}

bool SettingRegistry::merge_completions_json(const nlohmann::json& subtree) {
    if (!subtree.is_object())
        return false;
    for (auto it = subtree.begin(); it != subtree.end(); ++it) {
        nlohmann::json& node = tree_["commands"][it.key()];
        if (!node.is_object())
            node = nlohmann::json::object();
        merge_tree_node(node, it.value(), it.key());
    }
    // The index must describe the merged tree, not the subtree that was
    // offered: otherwise a blocked overwrite would still leak into help or
    // choices through the index.
    reindex();
    return true;
}

void SettingRegistry::clear_index() {
    key_help_.clear();
    key_man_.clear();
    key_children_.clear();
    command_choices_.clear();
    command_ranges_.clear();
    command_aliases_.clear();
    command_shorts_.clear();
    command_subcommands_.clear();
    shortcut_collisions_ = 0;
}

void SettingRegistry::reindex() {
    clear_index();
    if (!tree_.contains("commands") || !tree_["commands"].is_object())
        return;
    for (auto it = tree_["commands"].begin(); it != tree_["commands"].end(); ++it)
        index_node(it.value(), it.key());
    rebuild_shortcut_collisions();
}

void SettingRegistry::rebuild_shortcut_collisions() {
    shortcut_collisions_ = 0;
    std::set<std::string> seen;
    for (const auto& [path, short_name] : command_shorts_) {
        const std::size_t dot = path.rfind('.');
        const std::string parent = dot == std::string::npos ? std::string() : path.substr(0, dot);
        std::string key = parent;
        key.push_back('\x1f');
        key.append(short_name);
        if (!seen.insert(key).second)
            ++shortcut_collisions_;
    }
}

namespace {

struct ChildMatch {
    const nlohmann::json* node = nullptr;
    std::string name;
};

std::string node_short(const nlohmann::json& node, const std::string& name) {
    if (node.contains("short") && node["short"].is_string())
        return node["short"].get<std::string>();
    return name.empty() ? std::string() : name.substr(0, 1);
}

void add_child_match(std::vector<ChildMatch>& out, const nlohmann::json& node,
                     const std::string& name) {
    for (const auto& m : out)
        if (m.node == &node)
            return;
    out.push_back({&node, name});
}

void collect_glued(const nlohmann::json& children, const std::string& token, std::size_t depth,
                   std::vector<ChildMatch>& out) {
    for (auto it = children.begin(); it != children.end(); ++it) {
        if (!it.value().is_object())
            continue;
        const std::string short_name = node_short(it.value(), it.key());
        if (short_name.empty() || token.rfind(short_name, 0) != 0)
            continue;
        const std::string rest = token.substr(short_name.size());
        if (rest.empty()) {
            if (depth + 1 >= 2)
                add_child_match(out, it.value(), it.key());
            continue;
        }
        if (it.value().contains("children") && it.value()["children"].is_object())
            collect_glued(it.value()["children"], rest, depth + 1, out);
    }
}

void collect_prefix(const nlohmann::json& children, const std::string& token,
                    std::vector<ChildMatch>& out) {
    for (auto it = children.begin(); it != children.end(); ++it) {
        if (!it.value().is_object())
            continue;
        bool match = it.key().rfind(token, 0) == 0;
        const std::string short_name = node_short(it.value(), it.key());
        match = match || short_name.rfind(token, 0) == 0;
        if (!match && it.value().contains("aliases") && it.value()["aliases"].is_array()) {
            for (const auto& alias : it.value()["aliases"]) {
                if (alias.is_string() && alias.get<std::string>().rfind(token, 0) == 0) {
                    match = true;
                    break;
                }
            }
        }
        if (match)
            add_child_match(out, it.value(), it.key());
    }
}

std::vector<std::string> names_of(const std::vector<ChildMatch>& matches) {
    std::vector<std::string> out;
    for (const auto& m : matches)
        if (std::find(out.begin(), out.end(), m.name) == out.end())
            out.push_back(m.name);
    return out;
}

void collect_alias_matches(const nlohmann::json& children, const std::string& token,
                           std::vector<ChildMatch>& out) {
    for (auto it = children.begin(); it != children.end(); ++it) {
        if (!it.value().is_object() || !it.value().contains("aliases") ||
            !it.value()["aliases"].is_array())
            continue;
        for (const auto& alias : it.value()["aliases"])
            if (alias.is_string() && alias.get<std::string>() == token)
                add_child_match(out, it.value(), it.key());
    }
}

void collect_short_matches(const nlohmann::json& children, const std::string& token,
                           std::vector<ChildMatch>& out) {
    for (auto it = children.begin(); it != children.end(); ++it)
        if (it.value().is_object() && node_short(it.value(), it.key()) == token)
            add_child_match(out, it.value(), it.key());
}

const nlohmann::json* pick_child(const std::vector<ChildMatch>& matches,
                                 std::vector<std::string>& candidates) {
    if (matches.size() == 1)
        return matches.front().node;
    if (matches.size() > 1)
        candidates = names_of(matches);
    return nullptr;
}

const nlohmann::json* match_child(const nlohmann::json& children, const std::string& token,
                                  bool allow_single_short, std::vector<std::string>& candidates) {
    auto by_name = children.find(token);
    if (by_name != children.end() && by_name->is_object())
        return &*by_name;

    std::vector<ChildMatch> alias_matches;
    collect_alias_matches(children, token, alias_matches);
    if (!alias_matches.empty())
        return pick_child(alias_matches, candidates);

    if (token.size() > 1 || allow_single_short) {
        std::vector<ChildMatch> short_matches;
        collect_short_matches(children, token, short_matches);
        if (!short_matches.empty())
            return pick_child(short_matches, candidates);
    }

    std::vector<ChildMatch> glued_matches;
    collect_glued(children, token, 0, glued_matches);
    if (!glued_matches.empty())
        return pick_child(glued_matches, candidates);

    std::vector<ChildMatch> prefix_matches;
    collect_prefix(children, token, prefix_matches);
    return pick_child(prefix_matches, candidates);
}

} // namespace

void SettingRegistry::reset_completion_index() {
    tree_ = nlohmann::json::object();
    static_paths_.clear();
    legacy_paths_.clear();
    merge_collisions_ = 0;
    clear_index();
}

namespace {

struct LegacyExpansion {
    std::vector<std::string> tokens;
    bool applied = false;
    std::size_t target_len = 0;
};

LegacyExpansion expand_legacy(const std::map<std::string, std::vector<std::string>>& legacy,
                              const std::vector<std::string>& tokens) {
    LegacyExpansion out;
    out.tokens = tokens;
    const auto it = legacy.find(tokens.front());
    if (it == legacy.end())
        return out;
    out.applied = true;
    out.target_len = it->second.size();
    out.tokens = it->second;
    out.tokens.insert(out.tokens.end(), tokens.begin() + 1, tokens.end());
    return out;
}

std::size_t walk_command(const nlohmann::json& tree, const std::vector<std::string>& tokens,
                         SettingRegistry::CommandMatch& out) {
    const nlohmann::json* children = &tree["commands"];
    std::size_t walked = 0;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        std::vector<std::string> candidates;
        const nlohmann::json* node =
            match_child(*children, tokens[i], i + 1 < tokens.size(), candidates);
        if (!node) {
            if (!candidates.empty()) {
                out.ambiguous = true;
                out.candidates = std::move(candidates);
            }
            break;
        }
        out.node = node;
        walked = i + 1;
        if (node->contains("children") && (*node)["children"].is_object())
            children = &(*node)["children"];
        else
            break;
    }
    return walked;
}

} // namespace

// RED stubs (#345): the alias-layer surface must exist for the failing tests
// to compile; the GREEN pass replaces these bodies with the real layer.
std::size_t SettingRegistry::alias_collisions() const {
    return 0;
}

std::string SettingRegistry::alias_target_path(const std::string& key) const {
    (void)key;
    return {};
}

SettingRegistry::CommandMatch
SettingRegistry::resolve_command(const std::vector<std::string>& tokens) const {
    CommandMatch out;
    if (tokens.empty() || !tree_.contains("commands") || !tree_["commands"].is_object())
        return out;

    const LegacyExpansion expanded = expand_legacy(legacy_paths_, tokens);
    const std::size_t walked = walk_command(tree_, expanded.tokens, out);
    if (expanded.applied && walked < expanded.target_len)
        out.node = nullptr; // the alias target is not installed
    if (out.node && !out.ambiguous)
        out.consumed = expanded.applied ? 1 : walked;
    return out;
}

std::string SettingRegistry::short_for(const std::string& path) const {
    const std::string rk = resolve_key(path);
    const auto it = command_shorts_.find(rk);
    return it == command_shorts_.end() ? std::string() : it->second;
}

const std::vector<std::string>& SettingRegistry::aliases_for(const std::string& key) const {
    static const std::vector<std::string> empty;
    std::string rk = resolve_key(key);
    auto it = command_aliases_.find(rk);
    return it != command_aliases_.end() ? it->second : empty;
}

std::vector<std::string> SettingRegistry::top_level_aliases() const {
    std::vector<std::string> out;
    for (const auto& [path, aliases] : command_aliases_)
        if (path.find('.') == std::string::npos)
            for (const auto& a : aliases)
                out.push_back(a);
    return out;
}

const std::vector<std::string>& SettingRegistry::choices_for(const std::string& key) const {
    static const std::vector<std::string> empty;
    std::string rk = resolve_key(key);
    auto it = command_choices_.find(rk);
    return (it != command_choices_.end()) ? it->second : empty;
}

const std::vector<std::string>& SettingRegistry::subcommands_for(const std::string& cmd) const {
    static const std::vector<std::string> empty;
    auto it = command_subcommands_.find(cmd);
    return (it != command_subcommands_.end()) ? it->second : empty;
}

bool SettingRegistry::range_for(const std::string& key, double& lo, double& hi) const {
    std::string rk = resolve_key(key);
    auto it = command_ranges_.find(rk);
    if (it != command_ranges_.end()) {
        lo = it->second.first;
        hi = it->second.second;
        return true;
    }
    return false;
}

} // namespace tui
