#pragma once

#include "tui/model_picker.h"

#include <cstddef>
#include <string>
#include <vector>

namespace tui {

// The model picker's provider tabs.
//
// /set model lists every provider's catalogue as one flat list. That answers
// "what models can I use" but not "which of these come from where": the same id
// appears once per provider, and finding the ones you want means reading the
// provider column on every row.
//
// So the list gains tabs: All first (the union, which is what the drawer showed
// before), then one per provider -- the active one first, since it is the one in
// use. Left/Right move between tabs while the drawer is open and are given back
// the moment it closes, because Left/Right scroll history everywhere else.
//
// Pure state, no ncurses and no repository: the drawer renders whatever this
// says, which is what makes the arrow and selection rules testable.
struct ProviderTab {
    std::string provider; // empty for the "All" tab
    std::string label;    // "All", or the provider name
    std::size_t count = 0;
};

// The picker's visible state: which tab is open, what is typed, what is
// selected.
class TabbedRows {
public:
    // Build the tabs for `rows`, putting `active_provider`'s tab first and
    // filtering by `filter`. Rows are the full union: every provider's copy of a
    // model is its own row.
    static TabbedRows build(const std::vector<ProviderModel>& rows,
                            const std::string& active_provider, const std::string& filter);

    const std::vector<ProviderTab>& tabs() const noexcept { return tabs_; }

    // The tab names, for the drawer's tab strip.
    std::vector<std::string> tab_labels() const;

    // Rows on the selected tab, after the filter.
    std::vector<ProviderModel> visible_rows() const;

    // Left (-1) / Right (+1). Clamped: the ends do not wrap, because wrapping
    // makes one keystroke look like the drawer is stuck.
    void move_tab(int direction);

    void select_tab(std::size_t index);

    // The row cursor within the visible rows.
    void move_row(int delta);

    void select_visible(std::size_t index);

    std::size_t selected_tab() const noexcept { return selected_tab_; }
    std::size_t selected_row() const noexcept { return selected_row_; }

    // Re-filter in place, keeping the selection in range and dropping tab
    // selections that the new filter has made out of bounds.
    void set_filter(const std::string& filter);

    const std::string& filter() const noexcept { return filter_; }

    // True while the drawer owns Left/Right. Anything that closes the drawer
    // must call close(), or a Left press afterwards scrolls the scrollback
    // instead of doing nothing.
    bool hijacks_arrows() const noexcept { return open_; }
    void open();
    void close();

    // Every row, unfiltered and in the order given.
    const std::vector<ProviderModel>& all_rows() const noexcept { return rows_; }

private:
    std::vector<ProviderModel> rows_for_tab(std::size_t tab) const;
    void recount();
    void clamp_selection();

    std::vector<ProviderModel> rows_;
    std::vector<ProviderTab> tabs_;
    // One cursor per tab, so switching away and back returns to the row the
    // user was on rather than resetting to the top.
    std::vector<std::size_t> row_sel_;
    std::size_t selected_tab_ = 0;
    std::size_t selected_row_ = 0;
    std::string filter_;
    bool open_ = false;
};

} // namespace tui
