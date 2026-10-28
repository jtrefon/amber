
#include "tui/drawer_rows.h"

#include <algorithm>

namespace tui {

namespace {

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

// The drawer is only meaningful for a command line: every row it paints is
// reachable by typing "/" plus that path.
bool is_command_line(const std::string& input) {
    return !input.empty() && input.front() == '/';
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
                      const SettingRegistry& settings, bool show_short) {
    std::string label = name;
    const std::string short_name = show_short ? settings.short_for(full_key) : std::string();
    if (!short_name.empty()) {
        label += " (" + short_name + ")";
    } else if (show_short) {
        std::string target = settings.alias_target_path(full_key);
        if (!target.empty()) {
            std::replace(target.begin(), target.end(), '.', ' ');
            label += " (\u2192 /" + target + ")";
        }
    }
    std::string line = "  ";
    line += label;
    std::string h = settings.help_for(full_key);
    if (!h.empty()) {
        if (label.size() < 34)
            line.append(34 - label.size(), ' ');
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
    // The drawer belongs to a command line, and a command line starts with "/".
    // A shorter line has nothing to slice: returning an empty query keeps a
    // stale caller from throwing out of substring arithmetic.
    if (input.size() < 2)
        return q;
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

// The keys the drawer lists for a parsed query, and the prefix their full key
// needs. A partial that names a child with children of its own descends into
// it -- the rows are that namespace's children, so the partial is consumed
// rather than applied -- otherwise the partial filters this namespace's
// children. Rows and entry names share this, because Enter dispatches the entry
// name while the paint shows the row: they must index the same set.
std::vector<std::string> drawer_keys(const DrawerQuery& q, const std::vector<std::string>& kids,
                                     const SettingRegistry& settings, std::string& full_prefix) {
    full_prefix = q.ns.empty() ? std::string() : q.ns + ".";
    if (!q.partial.empty()) {
        const std::string sub_key = q.ns.empty() ? q.partial : q.ns + "." + q.partial;
        auto sub = settings.children_of(sub_key);
        if (!sub.empty()) {
            full_prefix = sub_key + ".";
            return sub;
        }
    }
    std::vector<std::string> keys;
    for (const auto& k : kids)
        if (drawer_row_matches(k, q.partial))
            keys.push_back(k);
    return keys;
}

std::vector<std::string> drawer_rows(const std::string& input, const SettingRegistry& settings,
                                     const std::string& provider) {
    // The drawer is a view of a command line. A line that is not one has no
    // rows -- not the top-level rows, which is what an empty namespace would
    // otherwise paint under a prompt the user is still typing.
    if (!is_command_line(input))
        return {};
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

    std::string full_prefix;
    const std::vector<std::string> keys = drawer_keys(q, kids, settings, full_prefix);
    for (const auto& k : keys) {
        if (!drawer_row_in_provider(k, provider))
            continue;
        rows.push_back(child_row(k, full_prefix + k, settings, q.ns.empty()));
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
    if (!is_command_line(input))
        return {};
    const DrawerQuery q = parse_drawer_input(input, settings);
    const auto kids = settings.children_of(q.ns);
    if (kids.empty())
        return {};
    std::string full_prefix;
    return drawer_keys(q, kids, settings, full_prefix);
}

} // namespace tui
