#include "tui/drawer_tabs.h"
#include "tui/drawer_rows.h"

#include <algorithm>

namespace tui {

namespace {

// How much room one tab may take before the strip gives up on fitting it.
constexpr std::size_t kMaxTabWidth = 18;

// Cut to at most `max` BYTES, never more. The ellipsis is three bytes of UTF-8,
// so appending it after a naive substr() made the result wider than the cap --
// which made a caller's `pad = cap - size` underflow into an enormous count.
std::string truncate(const std::string& s, std::size_t max) {
    if (s.size() <= max)
        return s;
    const std::string ellipsis = "\u2026";
    if (max <= ellipsis.size())
        return s.substr(0, max);
    return s.substr(0, max - ellipsis.size()) + ellipsis;
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
        if (i == selected) {
            strip.selected_mark_pos = mark_at;
            // The highlight covers the tab INCLUDING its count: it is the tab's
            // own text, so a strip painted with it cannot light up a neighbour.
            strip.selected_len = strip.line.size() - mark_at;
        }
        if (strip.line.size() >= budget)
            break;
    }
    if (strip.line.size() > budget)
        strip.line.resize(budget);
    // Truncation must not leave a highlight running past what is drawn.
    if (strip.selected_mark_pos != std::string::npos) {
        if (strip.selected_mark_pos >= strip.line.size())
            strip.selected_len = 0;
        else
            strip.selected_len =
                std::min(strip.selected_len, strip.line.size() - strip.selected_mark_pos);
    }
    // The strip is never blanked: the loop breaks once the line reaches the
    // budget, so a rendered tab's marker always sits before it, and the resize
    // trims only the tail. The highlight is still clamped above, because "the
    // marker is inside the line" does not say the tab's TEXT is.
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
        // truncate() guarantees <= kProviderColumn bytes, so this cannot
        // underflow -- but assert the invariant rather than trust it, since an
        // underflow here appends gigabytes of padding.
        if (name.size() < kProviderColumn)
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

// Rows of `rows` belonging to `want` that match `filter`. `want` empty means all.
std::size_t count_matching(const std::vector<std::string>& rows, const std::string& want,
                           const std::string& filter) {
    std::size_t n = 0;
    for (const auto& row : rows) {
        if (!drawer_row_in_provider(row, want))
            continue;
        if (drawer_row_matches(row, filter))
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
    // search land" rather than "how many models exist". The counting predicate is
    // the drawer's own (drawer_row_matches), so a count always equals the rows
    // that provider's tab will show.
    std::vector<ProviderTab> tabs;
    tabs.push_back({"", "All", count_matching(rows, "", filter)});

    // Active provider first: it is the one in use, so Enter without thinking does
    // not switch away from it.
    const auto push = [&](const std::string& p) {
        tabs.push_back({p, p, count_matching(rows, p, filter)});
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
