#include "tui/drawer_tabs.h"
#include "tests/test_util.h"

#include <string>
#include <vector>

using tui::DrawerTabStrip;

namespace {

DrawerTabStrip strip() {
    std::vector<tui::ProviderTab> tabs{
        {"", "All", 12},
        {"beta", "beta", 7},
        {"alpha", "alpha", 5},
    };
    return tui::make_tab_strip(tabs, 1);
}

} // namespace

// The strip is what tells the user the keys have changed meaning: without it,
// Left/Right appear to do nothing at all.
TEST(tab_strip_renders_every_tab_name) {
    const auto s = strip();
    ASSERT(s.line.find("All") != std::string::npos);
    ASSERT(s.line.find("beta") != std::string::npos);
    ASSERT(s.line.find("alpha") != std::string::npos);
}

TEST(tab_strip_marks_the_selected_tab) {
    const auto s = strip();
    ASSERT(s.selected_mark_pos != std::string::npos);
    // The marker belongs to the selected tab, so it must sit inside that label.
    ASSERT(s.selected_mark_pos >= s.line.find("beta"));
    ASSERT(s.selected_mark_pos < s.line.find("alpha"));
}

TEST(tab_strip_shows_the_match_count_per_tab) {
    const auto s = strip();
    ASSERT(s.line.find("7") != std::string::npos);
    ASSERT(s.line.find("5") != std::string::npos);
}

TEST(tab_strip_renders_zero_for_a_tab_with_no_matches) {
    // A provider with nothing matching keeps its tab, with a 0, so the user sees
    // it exists and is empty instead of watching it disappear.
    std::vector<tui::ProviderTab> tabs{{"", "All", 0}, {"beta", "beta", 0}};
    const auto s = tui::make_tab_strip(tabs, 1);
    ASSERT(s.line.find("beta") != std::string::npos);
    ASSERT(s.line.find('0') != std::string::npos);
}

TEST(tab_strip_truncates_on_a_narrow_terminal) {
    std::vector<tui::ProviderTab> tabs{{"", "All", 3}, {"a-very-long-provider-name", "a-very-long-provider-name", 1}};
    const auto s = tui::make_tab_strip(tabs, 0, 20);
    ASSERT(static_cast<int>(s.line.size()) <= 20);
}

TEST(tab_strip_renders_nothing_when_there_is_only_one_tab) {
    // A single "All" tab has nothing to switch to, so the strip is not drawn and
    // the arrow keys keep their normal meaning.
    std::vector<tui::ProviderTab> tabs{{"", "All", 4}};
    const auto s = tui::make_tab_strip(tabs, 0);
    ASSERT(s.line.empty());
    ASSERT(s.selected_mark_pos == std::string::npos);
}

// Deriving tabs from drawer rows. Data-driven: only the model feed emits
// composite "provider::model" keys, and the rule must not hardcode the command.

TEST(make_provider_tabs_derives_providers_from_composite_rows) {
    const auto tabs = tui::make_provider_tabs({"beta::large", "beta::small", "alpha::shared"},
                                              "beta", "");
    ASSERT(tabs.size() == 3);
    ASSERT(tabs[0].provider.empty());
    ASSERT(tabs[0].label == "All");
    ASSERT(tabs[0].count == 3);
    ASSERT(tabs[1].provider == "beta");
    ASSERT(tabs[1].count == 2);
    ASSERT(tabs[2].provider == "alpha");
    ASSERT(tabs[2].count == 1);
}

TEST(make_provider_tabs_puts_the_active_provider_first) {
    const auto tabs =
        tui::make_provider_tabs({"beta::large", "alpha::shared"}, "alpha", "");
    ASSERT(tabs[1].provider == "alpha");
    ASSERT(tabs[2].provider == "beta");
}

TEST(make_provider_tabs_returns_none_for_plain_command_names) {
    // An ordinary namespace: no composite keys, so no tabs, so the arrow keys
    // keep moving the caret.
    ASSERT(tui::make_provider_tabs({"open", "close", "resize"}, "", "").empty());
}

TEST(make_provider_tabs_returns_none_when_a_row_has_no_provider) {
    // A mixed list is not the model picker either.
    ASSERT(tui::make_provider_tabs({"beta::large", "orphan"}, "", "").empty());
}

TEST(make_provider_tabs_returns_none_for_a_single_provider) {
    // One provider has nothing to switch to.
    ASSERT(tui::make_provider_tabs({"beta::large", "beta::small"}, "", "").empty());
}

TEST(make_provider_tabs_returns_none_for_an_empty_or_single_row) {
    ASSERT(tui::make_provider_tabs({}, "", "").empty());
    ASSERT(tui::make_provider_tabs({"beta::large"}, "", "").empty());
}

TEST(make_provider_tabs_counts_matches_under_the_filter) {
    const auto tabs = tui::make_provider_tabs({"beta::large", "beta::small", "alpha::shared"},
                                              "beta", "shared");
    ASSERT(tabs[0].count == 1); // All
    ASSERT(tabs[1].count == 0); // beta
    ASSERT(tabs[2].count == 1); // alpha
}

TEST(make_provider_tabs_ignores_an_active_provider_that_is_not_listed) {
    // A provider deleted from disk, or a cold start: fall back to row order
    // rather than inventing a tab nothing can fill.
    const auto tabs = tui::make_provider_tabs({"beta::large", "alpha::shared"}, "ghost", "");
    ASSERT(tabs.size() == 3);
    ASSERT(tabs[1].provider == "beta");
}
