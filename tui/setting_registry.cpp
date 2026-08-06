
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
        if (s.key == key) return &s;
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

std::vector<std::string> SettingRegistry::complete(const std::string& prefix) const {
    std::vector<std::string> out;
    // The command tree is the authoritative structure. Query semantics:
    //   complete("")            → top-level keys
    //   complete("set")         → direct children paths of "set"
    //   complete("set.policy")  → direct children paths of "set.policy"
    // Direct children only — the drawer rows and the completion list must
    // stay 1:1 aligned for Enter dispatch.
    auto add = [&](const std::string& key) {
        if (std::find(out.begin(), out.end(), key) == out.end())
            out.push_back(key);
    };
    if (!tree_.contains("commands") || !tree_["commands"].is_object())
        return out;
    const nlohmann::json* node = &tree_["commands"];
    if (!prefix.empty()) {
        // Walk to the namespace node: first token is a top-level command,
        // every following token descends through a "children" map.
        size_t p = 0;
        while (p < prefix.size()) {
            size_t dot = prefix.find('.', p);
            std::string tok = (dot == std::string::npos)
                                  ? prefix.substr(p)
                                  : prefix.substr(p, dot - p);
            if (p == 0) {
                if (!node->contains(tok)) return out;
                node = &(*node)[tok];
            } else {
                if (!node->is_object() || !node->contains("children") ||
                    !(*node)["children"].is_object() ||
                    !(*node)["children"].contains(tok))
                    return out;
                node = &(*node)["children"][tok];
            }
            if (dot == std::string::npos) break;
            p = dot + 1;
        }
    }
    const nlohmann::json* kids = node;
    if (!prefix.empty()) {
        if (!node->contains("children") || !(*node)["children"].is_object())
            return out;
        kids = &(*node)["children"];
    }
    for (auto it = kids->begin(); it != kids->end(); ++it) {
        std::string child = prefix.empty() ? it.key() : prefix + "." + it.key();
        add(child);
    }
    return out;
}

std::string SettingRegistry::resolve_key(const std::string& key) const {
    auto indexed = [&](const std::string& k) {
        return key_help_.count(k) || key_man_.count(k) ||
               key_children_.count(k) || command_choices_.count(k) ||
               command_ranges_.count(k);
    };
    if (indexed(key)) return key;
    std::string g = "get." + key;
    if (indexed(g)) return g;
    std::string s = "set." + key;
    if (indexed(s)) return s;
    return "";
}

std::string SettingRegistry::help_for(const std::string& key) const {
    std::string rk = resolve_key(key);
    if (rk.empty() || !key_help_.count(rk)) return "";
    return key_help_.at(rk);
}

std::string SettingRegistry::man_for(const std::string& key) const {
    std::string rk = resolve_key(key);
    if (rk.empty() || !key_man_.count(rk)) return "";
    return key_man_.at(rk);
}

std::vector<std::string> SettingRegistry::children_of(const std::string& key) const {
    std::string rk = resolve_key(key);
    auto it = key_children_.find(rk);
    return (it != key_children_.end()) ? it->second : std::vector<std::string>();
}

void SettingRegistry::index_node(const nlohmann::json& node,
                                    const std::string& display_path) {
    if (!node.is_object()) return;

    auto idx = [&](const std::string& key, const std::string& help,
                   const std::string& man,
                   const std::vector<std::string>& choices,
                   double rlo, double rhi, bool has_range) {
        if (!help.empty()) key_help_[key] = help;
        if (!man.empty()) key_man_[key] = man;
        if (!choices.empty()) command_choices_[key] = choices;
        if (has_range) command_ranges_[key] = {rlo, rhi};
    };

    std::string help_text, man_text, action;
    std::vector<std::string> choices;
    double rlo = 0, rhi = 0;
    bool has_range = false;

    if (node.contains("help") && node["help"].is_string())
        help_text = node["help"].get<std::string>();
    if (node.contains("man") && node["man"].is_string())
        man_text = node["man"].get<std::string>();
    if (node.contains("action") && node["action"].is_string())
        action = node["action"].get<std::string>();
    std::vector<std::string> aliases;
    if (node.contains("aliases") && node["aliases"].is_array())
        for (const auto& a : node["aliases"])
            aliases.push_back(a.get<std::string>());

    if (node.contains("choices") && node["choices"].is_array())
        for (const auto& c : node["choices"])
            choices.push_back(c.get<std::string>());
    if (node.contains("range") && node["range"].is_array() && node["range"].size() == 2) {
        rlo = node["range"][0].get<double>();
        rhi = node["range"][1].get<double>();
        has_range = true;
    }

    if (!action.empty())
        idx(action, help_text, man_text, choices, rlo, rhi, has_range);

    // Namespaces are indexed by their FULL display path so get.<ns> and
    // set.<ns> never collide ("set.model" != "get.model"). Top-level
    // commands keep their bare name ("model", "policy").
    std::string key_path = display_path;
    if (!key_path.empty() &&
        (key_path.find('.') != std::string::npos ||
         !node.contains("children") || !help_text.empty())) {
        idx(key_path, help_text, man_text, choices, rlo, rhi, has_range);
        if (!aliases.empty()) command_aliases_[key_path] = aliases;
    }

    auto union_into = [](std::vector<std::string>& dst,
                         const std::vector<std::string>& src) {
        for (const auto& k : src)
            if (std::find(dst.begin(), dst.end(), k) == dst.end())
                dst.push_back(k);
    };

    if (!display_path.empty() && display_path.find('.') == std::string::npos &&
        node.contains("children") && node["children"].is_object()) {
        std::vector<std::string> subs;
        for (auto it = node["children"].begin(); it != node["children"].end(); ++it)
            subs.push_back(it.key());
        union_into(command_subcommands_[display_path], subs);
    }

    if (!key_path.empty() && node.contains("children") && node["children"].is_object()) {
        std::vector<std::string> kids;
        for (auto it = node["children"].begin(); it != node["children"].end(); ++it)
            kids.push_back(it.key());
        union_into(key_children_[key_path], kids);
    }

    if (node.contains("children") && node["children"].is_object()) {
        for (auto it = node["children"].begin(); it != node["children"].end(); ++it) {
            std::string child_display = display_path.empty()
                ? it.key()
                : display_path + "." + it.key();
            index_node(it.value(), child_display);
        }
    }
}

bool SettingRegistry::load_completions_json(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;

    using json = nlohmann::json;
    json root;
    try {
        f >> root;
    } catch (...) {
        return false;
    }

    if (root.contains("commands") && root["commands"].is_object()) {
        tree_["commands"] = root["commands"];
        for (auto it = root["commands"].begin(); it != root["commands"].end(); ++it)
            index_node(it.value(), it.key());
    }
    return true;
}

namespace {

// Deep-merge a subtree into the command tree: "children" objects are merged
// recursively (union), every other field is taken from the source. This keeps
// documented nodes (action/help/man + static children) alive when a live
// integration (MCP server, plugin, value feed) merges its branches in.
void merge_tree_node(nlohmann::json& dst, const nlohmann::json& src) {
    if (!dst.is_object()) { dst = src; return; }
    for (auto it = src.begin(); it != src.end(); ++it) {
        if (it.key() == "children" && it.value().is_object()) {
            if (!dst.contains("children") || !dst["children"].is_object())
                dst["children"] = nlohmann::json::object();
            for (auto cit = it.value().begin(); cit != it.value().end(); ++cit)
                merge_tree_node(dst["children"][cit.key()], cit.value());
        } else {
            dst[it.key()] = it.value();
        }
    }
}

} // namespace

bool SettingRegistry::merge_completions_json(const nlohmann::json& subtree) {
    if (!subtree.is_object()) return false;
    for (auto it = subtree.begin(); it != subtree.end(); ++it) {
        index_node(it.value(), it.key());
        nlohmann::json& node = tree_["commands"][it.key()];
        if (!node.is_object()) node = nlohmann::json::object();
        merge_tree_node(node, it.value());
    }
    return true;
}

void SettingRegistry::reset_completion_index() {
    tree_ = nlohmann::json::object();
    key_help_.clear();
    key_man_.clear();
    key_children_.clear();
    command_choices_.clear();
    command_ranges_.clear();
    command_aliases_.clear();
    command_subcommands_.clear();
}

const std::vector<std::string>& SettingRegistry::aliases_for(
    const std::string& key) const {
    static const std::vector<std::string> empty;
    std::string rk = resolve_key(key);
    auto it = command_aliases_.find(rk);
    return it != command_aliases_.end() ? it->second : empty;
}

std::vector<std::string> SettingRegistry::top_level_aliases() const {
    std::vector<std::string> out;
    for (const auto& [path, aliases] : command_aliases_)
        if (path.find('.') == std::string::npos)
            for (const auto& a : aliases) out.push_back(a);
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
        lo = it->second.first; hi = it->second.second;
        return true;
    }
    return false;
}

} // namespace tui
