// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

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

    // If the prefix contains a dot, we're completing WITHIN a namespace.
    size_t dot = prefix.find('.');
    if (dot != std::string::npos) {
        std::string ns = prefix.substr(0, dot);
        std::string sub = prefix.substr(dot + 1);
        for (const auto& s : settings_) {
            // Match keys that start with "ns." and the sub-prefix.
            std::string expected = ns + ".";
            if (s.key.rfind(expected, 0) == 0) {
                std::string tail = s.key.substr(expected.size());
                if (tail.rfind(sub, 0) == 0)
                    out.push_back(tail);
            }
        }
        return out;
    }

    // No dot: show namespace-level completions first.
    for (const auto& ns : namespaces()) {
        if (ns.rfind(prefix, 0) == 0)
            out.push_back(ns);  // namespace name — drawer adds visual marker
    }

    // Also match full dotted keys whose last path component starts with prefix.
    // Scan both code-registered settings_ AND JSON-derived key_help_.
    auto add_if_missing = [&](const std::string& key) {
        // Extract the leaf (last component).
        size_t last_dot = key.rfind('.');
        std::string leaf = (last_dot == std::string::npos) ? key : key.substr(last_dot + 1);
        if (!leaf.empty() && leaf.rfind(prefix, 0) == 0) {
            if (std::find(out.begin(), out.end(), key) == out.end())
                out.push_back(key);
        }
    };
    for (const auto& s : settings_)
        add_if_missing(s.key);
    for (const auto& [key, _] : key_help_)
        if (key.rfind("core.", 0) != 0 && key.rfind("os.", 0) != 0)
            add_if_missing(key);

    return out;
}

std::vector<std::string> SettingRegistry::children_of(const std::string& key) const {
    auto it = key_children_.find(key);
    return (it != key_children_.end()) ? it->second : std::vector<std::string>();
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

    // Walk the tree: index metadata by display path (dotted config key)
    // and by action path (core.* / os.*).
    std::function<void(const json&, const std::string&)> walk;
    walk = [&](const json& node, const std::string& display_path) {
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

        // Extract metadata from this node.
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
        if (node.contains("choices") && node["choices"].is_array())
            for (const auto& c : node["choices"])
                choices.push_back(c.get<std::string>());
        if (node.contains("range") && node["range"].is_array() && node["range"].size() == 2) {
            rlo = node["range"][0].get<double>();
            rhi = node["range"][1].get<double>();
            has_range = true;
        }

        // Index by action path (core.*, os.*).
        if (!action.empty())
            idx(action, help_text, man_text, choices, rlo, rhi, has_range);

        // Index by the config KEY path (strip command prefix from display path).
        // e.g. display_path = "get.detection.loop" → key_path = "detection.loop"
        //      display_path = "set.toolfold"       → key_path = "toolfold"
        // This matches the dotted keys used by the SettingRegistry.
        std::string key_path;
        if (!display_path.empty()) {
            size_t first_dot = display_path.find('.');
            if (first_dot != std::string::npos && first_dot + 1 < display_path.size())
                key_path = display_path.substr(first_dot + 1);
            else if (first_dot == std::string::npos)
                key_path = display_path;  // single-level, no prefix to strip
        }
        if (!key_path.empty() && (key_path.find('.') != std::string::npos ||
            !node.contains("children")))
            idx(key_path, help_text, man_text, choices, rlo, rhi, has_range);
        else if (!key_path.empty() && !help_text.empty())
            idx(key_path, help_text, man_text, choices, rlo, rhi, has_range);

        // Store subcommand names for commands that have children.
        // This is used by update_completions to provide subcommand completions.
        if (!display_path.empty() && display_path.find('.') == std::string::npos &&
            node.contains("children") && node["children"].is_object()) {
            std::vector<std::string> subs;
            for (auto it = node["children"].begin(); it != node["children"].end(); ++it)
                subs.push_back(it.key());
            command_subcommands_[display_path] = subs;
        }

        // Index child keys for namespace nodes at ALL levels (not just depth 1).
        if (!key_path.empty() && node.contains("children") && node["children"].is_object()) {
            std::vector<std::string> kids;
            for (auto it = node["children"].begin(); it != node["children"].end(); ++it)
                kids.push_back(it.key());
            key_children_[key_path] = kids;
        }

        // Recurse into children.
        if (node.contains("children") && node["children"].is_object()) {
            for (auto it = node["children"].begin(); it != node["children"].end(); ++it) {
                std::string child_display = display_path.empty()
                    ? it.key()
                    : display_path + "." + it.key();
                walk(it.value(), child_display);
            }
        }
    };

    if (root.contains("commands") && root["commands"].is_object()) {
        for (auto it = root["commands"].begin(); it != root["commands"].end(); ++it) {
            walk(it.value(), it.key());  // pass command name as display path
        }
    }

    return true;
}

const std::vector<std::string>& SettingRegistry::choices_for(const std::string& key) const {
    static const std::vector<std::string> empty;
    auto it = command_choices_.find(key);
    return (it != command_choices_.end()) ? it->second : empty;
}

const std::vector<std::string>& SettingRegistry::subcommands_for(const std::string& cmd) const {
    static const std::vector<std::string> empty;
    auto it = command_subcommands_.find(cmd);
    return (it != command_subcommands_.end()) ? it->second : empty;
}

bool SettingRegistry::range_for(const std::string& key, double& lo, double& hi) const {
    auto it = command_ranges_.find(key);
    if (it != command_ranges_.end()) {
        lo = it->second.first; hi = it->second.second;
        return true;
    }
    return false;
}

} // namespace tui
