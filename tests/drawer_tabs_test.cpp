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
    // ...and it covers exactly that tab, count included. Reverse-video switched
    // on around the whole line made every tab read as selected.
    ASSERT_EQ(s.line.substr(s.selected_mark_pos, s.selected_len), std::string("beta(7)"));
}

TEST(tab_strip_marker_at_ends_and_under_truncation) {
    // Selecting the FIRST tab starts the highlight at column 0.
    const auto first = tui::make_tab_strip({{"", "All", 3}, {"beta", "beta", 1}}, 0);
    ASSERT_EQ(first.selected_mark_pos, static_cast<std::size_t>(0));
    ASSERT_EQ(first.line.substr(0, first.selected_len), std::string("All(3)"));
    // Truncation keeps the highlight inside what is drawn.
    std::vector<tui::ProviderTab> wide{
        {"", "All", 3}, {"a-very-long-provider-name", "a-very-long-provider-name", 1}};
    const auto cut = tui::make_tab_strip(wide, 1, 20);
    ASSERT(cut.selected_mark_pos != std::string::npos);
    ASSERT(cut.selected_mark_pos + cut.selected_len <= cut.line.size());
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
    std::vector<tui::ProviderTab> tabs{
        {"", "All", 3}, {"a-very-long-provider-name", "a-very-long-provider-name", 1}};
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
    const auto tabs =
        tui::make_provider_tabs({"beta::large", "beta::small", "alpha::shared"}, "beta", "");
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
    const auto tabs = tui::make_provider_tabs({"beta::large", "alpha::shared"}, "alpha", "");
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
    const auto tabs =
        tui::make_provider_tabs({"beta::large", "beta::small", "alpha::shared"}, "beta", "shared");
    ASSERT(tabs[0].count == 1); // All
    ASSERT(tabs[1].count == 0); // beta
    ASSERT(tabs[2].count == 1); // alpha
}

// The count uses the drawer's own predicate, which is case-insensitive: "qwen"
// counts the capital-Q ids too, so the number on a tab is exactly the number of
// rows that tab will show.
TEST(make_provider_tabs_counts_case_insensitively) {
    const auto tabs = tui::make_provider_tabs(
        {"custom::/models/Qwen3.8-27B.gguf", "custom::qwen35-dense", "kilo::qwen/qwen3.8-max"},
        "custom", "qwen");
    ASSERT(tabs.size() == 3);
    ASSERT(tabs[0].count == 3); // All
    ASSERT(tabs[1].count == 2); // custom: both, whatever the case
    ASSERT(tabs[2].count == 1); // kilo
}

TEST(make_provider_tabs_ignores_an_active_provider_that_is_not_listed) {
    // A provider deleted from disk, or a cold start: fall back to row order
    // rather than inventing a tab nothing can fill.
    const auto tabs = tui::make_provider_tabs({"beta::large", "alpha::shared"}, "ghost", "");
    ASSERT(tabs.size() == 3);
    ASSERT(tabs[1].provider == "beta");
}

// The drawer row's provider column. A composite key "provider::model" puts the
// provider first in the text, so a model id reads as if it belonged to whichever
// provider is leftmost -- and on the All tab there is no leftmost.

TEST(model_row_shows_the_provider_in_its_own_column) {
    const auto row = tui::model_row("alpha", "shared/model", 200000);
    ASSERT(row.find("alpha") != std::string::npos);
    ASSERT(row.find("shared/model") != std::string::npos);
    // The provider is its own field: it starts the line, the model follows it,
    // and the id never carries the "provider::" prefix.
    ASSERT(row.find("::") == std::string::npos);
}

TEST(model_row_keeps_a_bare_id_bare) {
    // A row with no provider (from a hand-edited tree) still renders.
    const auto row = tui::model_row("", "plain-model", 0);
    ASSERT(row.find("plain-model") != std::string::npos);
}

TEST(model_row_shows_the_context_window_when_known) {
    ASSERT(tui::model_row("a", "m", 128000).find("128000") != std::string::npos);
    // Unknown context is omitted rather than shown as 0.
    ASSERT(tui::model_row("a", "m", 0).find("(0") == std::string::npos);
}

TEST(model_row_aligns_two_providers_so_the_models_line_up) {
    // A column only reads as a column if it is padded: comparing across providers
    // is the whole reason it exists.
    const auto short_p = tui::model_row("ai", "one", 0);
    const auto long_p = tui::model_row("a-very-long-provider", "two", 0);
    const auto model_col_short = short_p.find("one");
    const auto model_col_long = long_p.find("two");
    ASSERT(model_col_short != std::string::npos);
    ASSERT(model_col_long != std::string::npos);
    ASSERT_EQ(model_col_short, model_col_long);
}

// Paths the tab strip reaches only on a narrow terminal or an unusual tab list.
// Both were unreachable from the happy-path tests above, which is how a strip
// that renders nothing on a 40-column terminal would have shipped.

TEST(tab_names_returns_the_providers_in_order) {
    // CommandLine stores tab NAMES, so this is the seam between the two.
    const auto names = tui::tab_names({{"", "All", 3}, {"beta", "beta", 2}, {"alpha", "alpha", 1}});
    ASSERT(names.size() == 3);
    ASSERT(names[0].empty());
    ASSERT(names[1] == "beta");
    ASSERT(names[2] == "alpha");
}

TEST(model_row_truncates_a_provider_name_wider_than_the_column) {
    // Long enough to overflow the column: the row must stay one line and keep
    // its model, rather than pushing it off the end.
    const std::string long_name(60, 'x');
    const auto row = tui::model_row(long_name, "the-model", 0);
    ASSERT(row.find("the-model") != std::string::npos);
    // The truncated name ends with the ellipsis the strip uses for the same job.
    ASSERT(row.find("\xe2\x80\xa6") != std::string::npos);
}
