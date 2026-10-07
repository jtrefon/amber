#include "tui/provider_tabs.h"
#include "tests/test_util.h"

#include <algorithm>
#include <string>
#include <vector>

using tui::ProviderTab;
using tui::TabbedRows;

namespace {

// Two providers, three models between them, active is "beta".
TabbedRows sample() {
    std::vector<tui::ProviderModel> rows{
        {"beta", "beta-large", 100000},
        {"beta", "beta-small", 8000},
        {"alpha", "shared-model", 200000},
    };
    return tui::TabbedRows::build(rows, "beta", "");
}

} // namespace

// "All" comes first, so opening the drawer lands on everything rather than on
// whichever provider happened to be active.
TEST(provider_tabs_start_with_all) {
    const auto t = sample();
    ASSERT(t.tabs().size() == 3);
    ASSERT(t.tabs()[0].provider.empty());
    ASSERT(t.tabs()[0].label == "All");
    ASSERT(t.tabs()[0].count == 3);
}

TEST(provider_tabs_count_rows_per_provider) {
    const auto t = sample();
    ASSERT(t.tabs()[1].provider == "beta");
    ASSERT(t.tabs()[1].count == 2);
    ASSERT(t.tabs()[2].provider == "alpha");
    ASSERT(t.tabs()[2].count == 1);
}

// The active provider's tab leads the provider tabs: it is the one in use, and
// making it the default tab means Enter without thinking switches nothing.
TEST(provider_tabs_put_the_active_provider_first) {
    const auto t = sample();
    ASSERT(t.tabs()[1].provider == "beta");
}

TEST(provider_tabs_keep_every_provider_copy_separate) {
    // Two providers offering one id are two rows, not one merged row: picking
    // one switches provider, so the row has to say which provider it moves to.
    std::vector<tui::ProviderModel> rows{
        {"beta", "shared-model", 1000},
        {"alpha", "shared-model", 2000},
    };
    const auto t = tui::TabbedRows::build(rows, "beta", "");
    ASSERT(t.all_rows().size() == 2);
    ASSERT(t.all_rows()[0].provider == "beta");
    ASSERT(t.all_rows()[1].provider == "alpha");
}

TEST(provider_tabs_filter_to_the_selected_tab) {
    auto t = sample();
    t.select_tab(1); // beta
    ASSERT(t.visible_rows().size() == 2);
    ASSERT(t.visible_rows()[0].id == "beta-large");
}

TEST(provider_tabs_all_tab_shows_every_row) {
    auto t = sample();
    ASSERT(t.visible_rows().size() == 3);
}

TEST(provider_tabs_left_and_right_switch_tabs_and_stop_at_the_ends) {
    auto t = sample();
    ASSERT(t.selected_tab() == 0);
    t.move_tab(-1);
    ASSERT(t.selected_tab() == 0); // already leftmost
    t.move_tab(+1);
    ASSERT(t.selected_tab() == 1);
    t.move_tab(+1);
    ASSERT(t.selected_tab() == 2);
    t.move_tab(+1);
    ASSERT(t.selected_tab() == 2); // already rightmost
    t.move_tab(-1);
    ASSERT(t.selected_tab() == 1);
}

// Switching tab must not silently change which row the user was about to press
// Enter on: keep a selection per tab.
TEST(provider_tabs_remember_the_selection_per_tab) {
    auto t = sample();
    t.select_visible(1);
    ASSERT(t.selected_row() == 1);
    t.move_tab(+1);
    ASSERT(t.selected_row() == 0); // alpha's first row
    t.move_tab(-1);
    ASSERT(t.selected_row() == 1); // back to beta's second row
}

TEST(provider_tabs_selection_clamps_when_the_filter_shrinks) {
    auto t = sample();
    t.select_visible(1);
    t.set_filter("large");
    ASSERT(t.visible_rows().size() == 1);
    ASSERT(t.selected_row() == 0);
}

// Counts are per tab, so the user can see where a search landed before
// arrowing into it.
TEST(provider_tabs_report_matches_per_provider_while_filtering) {
    auto t = sample();
    t.set_filter("shared");
    ASSERT(t.visible_rows().size() == 1);
    ASSERT(t.tabs()[0].count == 1); // All
    ASSERT(t.tabs()[1].count == 0); // beta
    ASSERT(t.tabs()[2].count == 1); // alpha
}

TEST(provider_tabs_count_every_provider_even_when_it_matched_nothing) {
    // A provider with zero matches keeps its tab, so the user can see it exists
    // and has nothing rather than watching it disappear.
    auto t = sample();
    t.set_filter("shared");
    ASSERT(t.tabs().size() == 3);
    ASSERT(t.tabs()[1].count == 0);
}

TEST(provider_tabs_closing_returns_control_of_the_arrows) {
    // The drawer owns Left/Right only while it is open. Anything that closes it
    // must clear that claim, or a Left press in the scrollback scrolls history.
    auto t = sample();
    t.open(); // opening the drawer is what claims the arrows
    ASSERT(t.hijacks_arrows());
    t.close();
    ASSERT(!t.hijacks_arrows());
}
