#include "tui/drawer_tabs.h"

#include <algorithm>
#include <cctype>

namespace tui {

namespace {

// How much room one tab may take before the strip gives up on fitting it.
constexpr std::size_t kMaxTabWidth = 18;

std::string lowered(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string truncate(const std::string& s, std::size_t max) {
    if (s.size() <= max)
        return s;
    return s.substr(0, max > 1 ? max - 1 : 0) + "…";
}

} // namespace

DrawerTabStrip make_tab_strip(const std::vector<ProviderTab>& tabs, std::size_t selected,
                              int max_width) {
    DrawerTabStrip strip;
    // One tab is not a tab bar: claiming the arrow keys for a single "All" would
    // silently disable scrolling for nothing.
    if (tabs.size() < 2 || max_width <= 0)
        return strip;

    const auto budget = static_cast<std::size_t>(max_width);
    for (std::size_t i = 0; i < tabs.size(); ++i) {
        if (!strip.line.empty())
            strip.line += " ";
        const std::size_t mark_at = strip.line.size();
        // The count is what makes a zero-match tab readable rather than
        // mysterious, so it is part of the label, not decoration.
        strip.line += truncate(tabs[i].label, kMaxTabWidth);
        strip.line += "(" + std::to_string(tabs[i].count) + ")";
        if (i == selected)
            strip.selected_mark_pos = mark_at;
        if (strip.line.size() >= budget)
            break;
    }
    if (strip.line.size() > budget)
        strip.line.resize(budget);
    // Truncation can drop the selected marker; a strip that cannot say which tab
    // is active is worse than no strip.
    if (strip.selected_mark_pos != std::string::npos &&
        strip.selected_mark_pos >= strip.line.size()) {
        strip.line.clear();
        strip.selected_mark_pos = std::string::npos;
    }
    return strip;
}

namespace {

// A row names its provider when it is a composite key: the text before "::".
bool provider_of_row(const std::string& row, std::string& provider) {
    const auto sep = row.find("::");
    if (sep == std::string::npos || sep == 0)
        return false;
    provider = row.substr(0, sep);
    return true;
}

bool row_matches_filter(const std::string& row, const std::string& lower) {
    if (lower.empty())
        return true;
    std::string l = lowered(row);
    return l.find(lower) != std::string::npos;
}

} // namespace

namespace {

// Wide enough for a realistic provider name; beyond it the column is dropped
// rather than allowed to push the model off the row.
constexpr std::size_t kProviderColumn = 18;

} // namespace

std::string model_row(const std::string& provider, const std::string& id, int context) {
    std::string line = "  ";
    if (provider.empty()) {
        line += id;
    } else {
        const std::string name = truncate(provider, kProviderColumn);
        line += name;
        line.append(kProviderColumn - name.size(), ' ');
        line += "  ";
        line += id;
    }
    if (context > 0)
        line += "  (ctx " + std::to_string(context) + ")";
    return line;
}

std::vector<std::string> tab_names(const std::vector<ProviderTab>& tabs) {
    std::vector<std::string> out;
    out.reserve(tabs.size());
    for (const auto& t : tabs)
        out.push_back(t.provider);
    return out;
}

// The distinct providers named by `rows`, or empty when the rows are not the
// model picker. Every row must name one: a partial list would take the arrow
// keys away from an ordinary drawer, which is the failure this avoids.
std::vector<std::string> providers_in_rows(const std::vector<std::string>& rows) {
    std::vector<std::string> providers;
    for (const auto& row : rows) {
        std::string p;
        if (!provider_of_row(row, p))
            return {};
        if (std::find(providers.begin(), providers.end(), p) == providers.end())
            providers.push_back(p);
    }
    return providers;
}

// Rows of `rows` belonging to `want` that match `lower`. `want` empty means all.
std::size_t count_matching(const std::vector<std::string>& rows, const std::string& want,
                           const std::string& lower) {
    std::size_t n = 0;
    for (const auto& row : rows) {
        std::string p;
        if (!want.empty() && (!provider_of_row(row, p) || p != want))
            continue;
        if (row_matches_filter(row, lower))
            ++n;
    }
    return n;
}

std::vector<ProviderTab> make_provider_tabs(const std::vector<std::string>& rows,
                                            const std::string& active_provider,
                                            const std::string& filter) {
    // Fewer than two providers is nothing to switch between, so no tabs and the
    // arrow keys keep their normal meaning.
    if (rows.size() < 2)
        return {};
    std::vector<std::string> providers = providers_in_rows(rows);
    if (providers.size() < 2)
        return {};

    // Count per provider over the FILTERED rows, so a count answers "where did my
    // search land" rather than "how many models exist".
    const std::string lower = lowered(filter);
    std::vector<ProviderTab> tabs;
    tabs.push_back({"", "All", count_matching(rows, "", lower)});

    // Active provider first: it is the one in use, so Enter without thinking does
    // not switch away from it.
    const auto push = [&](const std::string& p) {
        tabs.push_back({p, p, count_matching(rows, p, lower)});
    };
    const auto is_active =
        std::find(providers.begin(), providers.end(), active_provider) != providers.end();
    if (is_active)
        push(active_provider);
    for (const auto& p : providers)
        if (p != active_provider)
            push(p);
    return tabs;
}

} // namespace tui
