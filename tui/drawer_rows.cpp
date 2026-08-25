
#include "tui/drawer_rows.h"

#include <algorithm>

namespace tui {

namespace {

// Append "[choice|choice]" or "[lo-hi]" to a row for leaf settings.
void append_choices(std::string& line, const std::string& key,
                    const SettingRegistry& settings) {
    const auto& ch = settings.choices_for(key);
    if (!ch.empty()) {
        line += "  [";
        for (size_t i = 0; i < ch.size(); ++i) {
            if (i > 0) line += "|";
            line += ch[i];
        }
        line += "]";
        return;
    }
    double rlo, rhi;
    if (settings.range_for(key, rlo, rhi))
        line += "  [" + std::to_string((int)rlo) + "-" +
                std::to_string((int)rhi) + "]";
}

// Convert "get policy mode" → "get.policy.mode" (namespaces are indexed
// by their full display path).
std::string dotted_path(const std::string& space_separated) {
    std::string out;
    size_t p = 0;
    while (p < space_separated.size()) {
        size_t spc = space_separated.find(' ', p);
        std::string tok = (spc == std::string::npos)
                              ? space_separated.substr(p)
                              : space_separated.substr(p, spc - p);
        if (!out.empty()) out += ".";
        out += tok;
        if (spc == std::string::npos) break;
        p = spc + 1;
    }
    return out;
}

std::string child_row(const std::string& name, const std::string& full_key,
                      const SettingRegistry& settings) {
    std::string line = "  ";
    line += name;
    std::string h = settings.help_for(full_key);
    if (!h.empty()) {
        if (name.size() < 34) line.append(34 - name.size(), ' ');
        line += "  ";
        line += h;
    }
    append_choices(line, full_key, settings);
    return line;
}

} // namespace

std::vector<std::string> drawer_rows(const std::string& input,
                                     const SettingRegistry& settings) {
    std::vector<std::string> rows;

    // Extract the namespace path: "/get policy mode" → ns "get.policy",
    // partial "mode". A missing space means the whole token is partial.
    std::string ns_path, partial;
    size_t sp = input.find(' ', 1);
    if (sp != std::string::npos) {
        ns_path = input.substr(1);
        while (!ns_path.empty() && ns_path.back() == ' ') ns_path.pop_back();
        size_t last_sp = ns_path.rfind(' ');
        if (last_sp != std::string::npos) {
            partial = ns_path.substr(last_sp + 1);
            ns_path.resize(last_sp);
        }
    } else {
        ns_path = input.substr(1);
    }
    std::string ns = dotted_path(ns_path);

    auto kids = settings.children_of(ns);
    if (kids.empty()) {
        // Leaf namespace: show its own help as the single hint row.
        std::string h = settings.help_for(ns);
        if (!h.empty()) {
            std::string line = "  ";
            line += h;
            append_choices(line, ns, settings);
            rows.push_back(line);
        }
        return rows;
    }

    // If the partial exactly matches a child that has its own children,
    // descend into that child's namespace.
    if (!partial.empty()) {
        std::string sub_key = ns.empty() ? partial : ns + "." + partial;
        auto sub = settings.children_of(sub_key);
        if (!sub.empty()) {
            for (const auto& sk : sub) {
                std::string full_key;
                full_key.reserve(sub_key.size() + 1 + sk.size());
                full_key += sub_key;
                full_key += ".";
                full_key += sk;
                rows.push_back(child_row(sk, full_key, settings));
            }
            return rows;
        }
    }

    for (const auto& k : kids) {
        if (!partial.empty() && k.rfind(partial, 0) != 0) continue;
        std::string full_key;
        full_key.reserve(ns.size() + 1 + k.size());
        if (!ns.empty()) {
            full_key += ns;
            full_key += ".";
        }
        full_key += k;
        rows.push_back(child_row(k, full_key, settings));
    }
    if (rows.empty())
        rows.emplace_back("  (no matching option  -  Esc to cancel)");
    return rows;
}

std::vector<std::string> drawer_entry_names(const std::string& input,
                                            const SettingRegistry& settings) {
    std::string ns_path, partial;
    size_t sp = input.find(' ', 1);
    if (sp != std::string::npos) {
        ns_path = input.substr(1);
        while (!ns_path.empty() && ns_path.back() == ' ') ns_path.pop_back();
        size_t last_sp = ns_path.rfind(' ');
        if (last_sp != std::string::npos) {
            partial = ns_path.substr(last_sp + 1);
            ns_path.resize(last_sp);
        }
    } else {
        ns_path = input.substr(1);
    }
    std::string ns = dotted_path(ns_path);
    auto kids = settings.children_of(ns);
    if (kids.empty()) return {};
    if (!partial.empty()) {
        std::string sub_key = ns.empty() ? partial : ns + "." + partial;
        auto sub = settings.children_of(sub_key);
        if (!sub.empty()) {
            return sub;
        }
    }
    std::vector<std::string> out;
    for (const auto& k : kids) {
        if (!partial.empty() && k.rfind(partial, 0) != 0) continue;
        out.push_back(k);
    }
    return out;
}

} // namespace tui
