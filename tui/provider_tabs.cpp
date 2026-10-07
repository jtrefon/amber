#include "tui/provider_tabs.h"

#include <algorithm>

namespace tui {

namespace {

// Substring match, case-insensitive: the filter is typed by hand.
bool matches(const ProviderModel& row, const std::string& lower_filter) {
    if (lower_filter.empty())
        return true;
    std::string id = row.id, provider = row.provider;
    std::transform(id.begin(), id.end(), id.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(provider.begin(), provider.end(), provider.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return id.find(lower_filter) != std::string::npos ||
           provider.find(lower_filter) != std::string::npos;
}

std::string lowered(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// "All" first, then the active provider, then the rest in the order they appear.
std::vector<ProviderTab> make_tabs(const std::vector<ProviderModel>& rows,
                                   const std::string& active) {
    std::vector<ProviderTab> tabs;
    tabs.push_back({"", "All", 0});

    const auto push = [&](const std::string& provider) {
        for (auto& t : tabs)
            if (t.provider == provider)
                return;
        tabs.push_back({provider, provider, 0});
    };
    if (!active.empty())
        push(active);
    for (const auto& row : rows)
        push(row.provider);
    return tabs;
}

} // namespace

TabbedRows TabbedRows::build(const std::vector<ProviderModel>& rows,
                             const std::string& active_provider, const std::string& filter) {
    TabbedRows t;
    t.rows_ = rows;
    t.filter_ = filter;
    t.tabs_ = make_tabs(rows, active_provider);
    t.recount();
    t.row_sel_.assign(t.tabs_.size(), 0);
    t.clamp_selection();
    return t;
}

std::vector<std::string> TabbedRows::tab_labels() const {
    std::vector<std::string> out;
    out.reserve(tabs_.size());
    for (const auto& tab : tabs_)
        out.push_back(tab.label);
    return out;
}

void TabbedRows::recount() {
    const std::string lower = lowered(filter_);
    for (auto& tab : tabs_) {
        tab.count = 0;
        for (const auto& row : rows_) {
            if (!tab.provider.empty() && row.provider != tab.provider)
                continue;
            if (matches(row, lower))
                ++tab.count;
        }
    }
}

std::vector<ProviderModel> TabbedRows::rows_for_tab(std::size_t tab) const {
    std::vector<ProviderModel> out;
    if (tab >= tabs_.size())
        return out;
    const std::string& want = tabs_[tab].provider;
    const std::string lower = lowered(filter_);
    for (const auto& row : rows_) {
        if (!want.empty() && row.provider != want)
            continue;
        if (matches(row, lower))
            out.push_back(row);
    }
    return out;
}

std::vector<ProviderModel> TabbedRows::visible_rows() const {
    return rows_for_tab(selected_tab_);
}

void TabbedRows::move_tab(int direction) {
    if (tabs_.empty() || direction == 0)
        return;
    const long long next = static_cast<long long>(selected_tab_) + direction;
    // Clamp rather than wrap: a wrapped Left looks like the drawer is stuck.
    if (next < 0 || next >= static_cast<long long>(tabs_.size()))
        return;
    select_tab(static_cast<std::size_t>(next));
}

void TabbedRows::select_tab(std::size_t index) {
    if (index >= tabs_.size())
        return;
    // Stash the outgoing tab's cursor so returning to it resumes there.
    if (selected_tab_ < row_sel_.size())
        row_sel_[selected_tab_] = selected_row_;
    selected_tab_ = index;
    selected_row_ = index < row_sel_.size() ? row_sel_[index] : 0;
    clamp_selection();
}

void TabbedRows::move_row(int delta) {
    const auto rows = visible_rows();
    if (rows.empty() || delta == 0)
        return;
    const long long next = static_cast<long long>(selected_row_) + delta;
    if (next < 0 || next >= static_cast<long long>(rows.size()))
        return;
    selected_row_ = static_cast<std::size_t>(next);
    if (selected_tab_ < row_sel_.size())
        row_sel_[selected_tab_] = selected_row_;
}

void TabbedRows::select_visible(std::size_t index) {
    const auto rows = visible_rows();
    if (index >= rows.size())
        return;
    selected_row_ = index;
    if (selected_tab_ < row_sel_.size())
        row_sel_[selected_tab_] = selected_row_;
}

void TabbedRows::clamp_selection() {
    if (selected_tab_ >= tabs_.size())
        selected_tab_ = tabs_.empty() ? 0 : tabs_.size() - 1;
    const std::size_t visible = visible_rows().size();
    if (visible == 0) {
        selected_row_ = 0;
    } else if (selected_row_ >= visible) {
        // The filter shrank the list under the cursor; stay in range instead of
        // pointing past the end.
        selected_row_ = visible - 1;
    }
    if (selected_tab_ < row_sel_.size())
        row_sel_[selected_tab_] = selected_row_;
}

void TabbedRows::set_filter(const std::string& filter) {
    filter_ = filter;
    recount();
    clamp_selection();
}

void TabbedRows::open() {
    open_ = true;
}

void TabbedRows::close() {
    open_ = false;
}

} // namespace tui
