#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "tui/session_browser_core.h"
#include "tui/keys.h"
#include "tests/minitest.h"

namespace {

using Action = tui::SessionBrowserCore::Result::Action;

std::vector<tui::BrowserItem> sample_items() {
    return {
        {"id1", "alpha", 1797000000},                               // same day pair
        {"id2", "beta", 1797100000},  {"id3", "gamma", 1750000000}, // older day
        {"id4", "delta", 1751000000}, {"id5", "epsilon", 1740000000}, {"id6", "zeta", 1741000000},
        {"id7", "eta", 1730000000},   {"id8", "theta", 1731000000},
    };
}

} // namespace

// ── Geometry: search bar sits on the last inner row, below the list ──

TEST(layout_search_bar_below_list) {
    auto lay = tui::browser_layout(24);
    ASSERT_EQ(lay.list_h, 21);     // inner minus header-free list area
    ASSERT_EQ(lay.search_row, 22); // last inner row (never overlaps list)
}

TEST(layout_small_dialog_keeps_one_list_row) {
    auto lay = tui::browser_layout(5);
    ASSERT(lay.list_h >= 1);
    ASSERT_EQ(lay.search_row, 3);
}

// ── Navigation: arrows move selection, never close ──

TEST(down_moves_selection_and_stays_open) {
    tui::SessionBrowserCore core(sample_items(), 5);
    auto r = core.key(tui::keys::kDown);
    ASSERT(r.action == Action::None);
    ASSERT_EQ(core.sel(), 2);
}

TEST(up_at_top_is_noop_and_stays_open) {
    tui::SessionBrowserCore core(sample_items(), 5);
    auto r = core.key(tui::keys::kUp);
    ASSERT(r.action == Action::None);
    ASSERT_EQ(core.sel(), 1); // sel starts on first session row (below its date header)
}

TEST(page_down_jumps_by_page) {
    tui::SessionBrowserCore core(sample_items(), 3);
    core.key(tui::keys::kNPage);
    ASSERT(core.sel() >= 4);
    ASSERT(core.key(tui::keys::kNPage).action == Action::None);
    core.key(tui::keys::kNPage);
    core.key(tui::keys::kNPage); // clamp at end
    ASSERT(core.sel() <= core.display_count() - 1);
}

TEST(page_up_jumps_back) {
    tui::SessionBrowserCore core(sample_items(), 3);
    core.key(tui::keys::kPPage);
    core.key(tui::keys::kNPage);
    core.key(tui::keys::kNPage);
    int before = core.sel();
    core.key(tui::keys::kPPage);
    ASSERT(core.sel() < before);
}

// ── Filter ──

TEST(printable_extends_filter_and_resets_selection) {
    tui::SessionBrowserCore core(sample_items(), 5);
    core.key('a');
    ASSERT_EQ(core.filter(), "a");
    ASSERT_EQ(core.sel(), 1); // snapped back to first session row
}

TEST(backspace_pops_filter) {
    tui::SessionBrowserCore core(sample_items(), 5);
    core.key('a');
    core.key('l');
    core.key(tui::keys::kBackspace);
    ASSERT_EQ(core.filter(), "a");
}

// ── Enter / Delete snap through date headers to real sessions ──

TEST(load_index_skips_date_header_rows) {
    tui::SessionBrowserCore core(sample_items(), 8);
    ASSERT_EQ(core.display_kind(0), 0); // row 0 is a date header
    ASSERT_EQ(core.display_kind(1), 1); // row 1 is a session
    ASSERT_EQ(core.load_index(), 0);    // snapped sel resolves to items[0]
}

TEST(delete_targets_snapped_session) {
    tui::SessionBrowserCore core(sample_items(), 8);
    auto r = core.key(tui::keys::kDelete);
    ASSERT(r.delete_pending);
    ASSERT_EQ(core.load_index(), 0);
}

// ── Enter accepts the selection, Esc dismisses: the host must tell them apart ──

TEST(enter_accepts_the_selection) {
    tui::SessionBrowserCore core(sample_items(), 5);
    auto r = core.key('\n');
    ASSERT(r.action == Action::Accept);
}

TEST(esc_cancels) {
    tui::SessionBrowserCore core(sample_items(), 5);
    auto r = core.key(27);
    ASSERT(r.action == Action::Cancel);
}

int main() {
    layout_search_bar_below_list();
    layout_small_dialog_keeps_one_list_row();
    down_moves_selection_and_stays_open();
    up_at_top_is_noop_and_stays_open();
    page_down_jumps_by_page();
    page_up_jumps_back();
    printable_extends_filter_and_resets_selection();
    backspace_pops_filter();
    load_index_skips_date_header_rows();
    delete_targets_snapped_session();
    enter_accepts_the_selection();
    esc_cancels();

    if (failed)
        std::cout << "FAILED (" << failed << " failures)\n";
    else
        std::cout << "ALL PASSED\n";
    return failed ? 1 : 0;
}
