
#include "tui/drawer_rows.h"

#include <algorithm>

namespace tui {

namespace {

// The one filter predicate both drawer_rows() and drawer_entry_names() apply.
//
// Keys are full ids of the form "vendor/name" for provider catalogues, so a
// prefix test alone can only ever match the vendor half: typing "space" for
// stealth/space-bunny-free is not a prefix of anything, and the row became
// unreachable rather than merely awkward to find. Match the prefix (so
// stepping through a list with single characters stays in order) or any
// substring of the id, which is what ListState already does.
//
// Both view functions call this. They must agree: CommandLine dispatches
// Enter on drawer_entry_names()[sel] while the renderer paints
// drawer_rows(), so a divergence selects a row the user cannot see.
bool entry_matches(const std::string& key, const std::string& partial) {
    if (partial.empty())
        return true;
    if (key.rfind(partial, 0) == 0)
        return true;
    return key.find(partial) != std::string::npos;
}

// Append "[choice|choice]" or "[lo-hi]" to a row for leaf settings.
void append_choices(std::string& line, const std::string& key, const SettingRegistry& settings) {
    const auto& ch = settings.choices_for(key);
    if (!ch.empty()) {
        line += "  [";
        for (size_t i = 0; i < ch.size(); ++i) {
            if (i > 0)
                line += "|";
            line += ch[i];
        }
        line += "]";
        return;
    }
    double rlo, rhi;
    if (settings.range_for(key, rlo, rhi))
        line += "  [" + std::to_string((int)rlo) + "-" + std::to_string((int)rhi) + "]";
}

// Convert "get policy mode" → "get.policy.mode" (namespaces are indexed
// by their full display path).
std::string dotted_path(const std::string& space_separated) {
    std::string out;
    size_t p = 0;
    while (p < space_separated.size()) {
        size_t spc = space_separated.find(' ', p);
        std::string tok = (spc == std::string::npos) ? space_separated.substr(p)
                                                     : space_separated.substr(p, spc - p);
        if (!out.empty())
            out += ".";
        out += tok;
        if (spc == std::string::npos)
            break;
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
        if (name.size() < 34)
            line.append(34 - name.size(), ' ');
        line += "  ";
        line += h;
    }
    append_choices(line, full_key, settings);
    return line;
}

} // namespace

// The namespace path and the partial token the user is filtering on.
struct DrawerQuery {
    std::string ns;
    std::string partial;
};

// Extract the namespace path: "/get policy mode" → ns "get.policy", partial
// "mode". A missing space means the whole token is partial.
DrawerQuery parse_drawer_input(const std::string& input, const SettingRegistry& settings) {
    DrawerQuery q;
    std::string ns_path = input.substr(1);
    const size_t sp = input.find(' ', 1);
    if (sp == std::string::npos) {
        // No space yet — the user is typing the FIRST token after "/". If it
        // names an exact top-level command/namespace (e.g. "/get"), descend
        // into it. If it is only a PARTIAL (e.g. "/c"), treat it as a filter
        // against the top-level command list: children_of("") returns the
        // top-level keys, and the caller filters them by this prefix below.
        if (ns_path.empty())
            return q;
        auto top = settings.complete("");
        bool exact = false;
        for (const auto& t : top)
            if (t == ns_path) {
                exact = true;
                break;
            }
        if (!exact) {
            q.partial = ns_path;
            return q;
        }
        q.ns = dotted_path(ns_path);
        return q;
    }
    while (!ns_path.empty() && ns_path.back() == ' ')
        ns_path.pop_back();
    const size_t last_sp = ns_path.rfind(' ');
    if (last_sp != std::string::npos) {
        q.partial = ns_path.substr(last_sp + 1);
        ns_path.resize(last_sp);
    }
    q.ns = dotted_path(ns_path);
    return q;
}

std::vector<std::string> drawer_rows(const std::string& input, const SettingRegistry& settings) {
    const DrawerQuery q = parse_drawer_input(input, settings);
    std::vector<std::string> rows;
    const auto kids = settings.children_of(q.ns);
    if (kids.empty()) {
        // Leaf namespace: show its own help as the single hint row.
        std::string h = settings.help_for(q.ns);
        if (!h.empty()) {
            std::string line = "  ";
            line += h;
            append_choices(line, q.ns, settings);
            rows.push_back(line);
        }
        return rows;
    }

    // If the partial exactly matches a child that has its own children,
    // descend into that child's namespace.
    if (!q.partial.empty()) {
        const std::string sub_key = q.ns.empty() ? q.partial : q.ns + "." + q.partial;
        auto sub = settings.children_of(sub_key);
        if (!sub.empty()) {
            for (const auto& sk : sub) {
                std::string full_key = sub_key;
                full_key += ".";
                full_key += sk;
                rows.push_back(child_row(sk, full_key, settings));
            }
            return rows;
        }
    }

    for (const auto& k : kids) {
        if (!entry_matches(k, q.partial))
            continue;
        std::string full_key = q.ns.empty() ? std::string() : q.ns + ".";
        full_key += k;
        rows.push_back(child_row(k, full_key, settings));
    }
    if (rows.empty())
        rows.emplace_back("  (no matching option  -  Esc to cancel)");
    return rows;
}

std::pair<int, int> drawer_window(int count, int max_visible, int sel) {
    const int n = std::max(0, count);
    const int vis = std::max(1, max_visible);
    if (n == 0)
        return {0, 0};
    int s = std::min(std::max(0, sel), n - 1);
    // Keep the selection inside the window, without scrolling past either end.
    int first = 0;
    if (s >= vis)
        first = std::min(s - vis + 1, n - vis);
    return {first, std::min(vis, n)};
}

std::vector<std::string> drawer_entry_names(const std::string& input,
                                            const SettingRegistry& settings) {
    const DrawerQuery q = parse_drawer_input(input, settings);
    const auto kids = settings.children_of(q.ns);
    if (kids.empty())
        return {};
    if (!q.partial.empty()) {
        const std::string sub_key = q.ns.empty() ? q.partial : q.ns + "." + q.partial;
        auto sub = settings.children_of(sub_key);
        if (!sub.empty())
            return sub;
    }
    std::vector<std::string> out;
    for (const auto& k : kids) {
        if (!entry_matches(k, q.partial))
            continue;
        out.push_back(k);
    }
    return out;
}

} // namespace tui
