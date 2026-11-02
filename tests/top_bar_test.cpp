// The top rail's pure view model: tab layout, clock and health pulse. No
// ncurses, no terminal, so the arithmetic is asserted directly.

#include "tui/top_bar.h"
#include "tui/textutil.h"
#include "test_util.h"

#include <ctime>
#include <string>
#include <vector>

using tui::top_bar::Layout;
using tui::top_bar::Role;
using tui::top_bar::Tab;

namespace {

std::vector<Tab> two_tabs() {
    return {
        {1, "amber-agent", true, false, tui::top_bar::Attention::None},
        {2, "docs", false, false, tui::top_bar::Attention::None},
    };
}

const tui::top_bar::Span* find_role(const Layout& layout, Role role) {
    for (const auto& span : layout.spans)
        if (span.role == role)
            return &span;
    return nullptr;
}

int layout_width(const Layout& layout) {
    int width = 0;
    for (const auto& span : layout.spans)
        width += tui::text::display_cols(span.text);
    return width;
}

std::tm local_tm(int hour, int minute, int second) {
    std::tm tm{};
    tm.tm_year = 126; // 2026
    tm.tm_mon = 8;    // September
    tm.tm_mday = 12;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    tm.tm_isdst = -1;
    return tm;
}

} // namespace

TEST(top_bar_marks_the_app_and_the_active_tab) {
    const Layout layout = tui::top_bar::layout(two_tabs(), 80);
    ASSERT_FALSE(layout.spans.empty());
    ASSERT_EQ(layout.spans.front().text, std::string("\u25c6"));
    ASSERT(layout.spans.front().role == Role::AppMark);

    const auto* active = find_role(layout, Role::ActiveTab);
    ASSERT(active != nullptr);
    ASSERT(active->text.find("amber-agent") != std::string::npos);
    ASSERT(layout.active_start > 0);
    ASSERT(layout.active_width == tui::text::display_cols(active->text));
}

TEST(top_bar_records_clickable_tab_hits) {
    const Layout layout = tui::top_bar::layout(two_tabs(), 80);
    ASSERT_EQ(layout.hits.size(), 2u);
    ASSERT_EQ(layout.hits[0].id, 1u);
    ASSERT_EQ(layout.hits[1].id, 2u);
    ASSERT_EQ(layout.hits[0].start, layout.active_start);
    ASSERT_EQ(layout.hits[0].width, layout.active_width);
    for (const auto& hit : layout.hits) {
        ASSERT(hit.start > 0);
        ASSERT(hit.width > 0);
    }
}

TEST(top_bar_uses_caps_for_tabs) {
    const Layout layout = tui::top_bar::layout(two_tabs(), 80);
    const auto* inactive = find_role(layout, Role::InactiveTab);
    ASSERT(inactive != nullptr);
    ASSERT(inactive->text.find("\u2590") != std::string::npos);
    ASSERT(inactive->text.find("\u258c") != std::string::npos);
}

TEST(top_bar_ascii_fallback_is_one_column) {
    ASSERT_EQ(tui::top_bar::app_mark(false), std::string("*"));
    ASSERT_EQ(tui::top_bar::tab_open(false), std::string("["));
    ASSERT_EQ(tui::top_bar::tab_close(false), std::string("]"));
    ASSERT_EQ(tui::text::display_cols(tui::top_bar::app_mark(false)), 1);
    ASSERT_EQ(tui::text::display_cols(tui::top_bar::tab_open(false)), 1);
}

TEST(top_bar_keeps_the_active_tab_visible_when_narrow) {
    std::vector<Tab> tabs;
    for (int i = 1; i <= 8; ++i)
        tabs.push_back({static_cast<std::size_t>(i), "s" + std::to_string(i), i == 5, false,
                        tui::top_bar::Attention::None});
    const Layout layout = tui::top_bar::layout(tabs, 40);
    ASSERT(layout_width(layout) <= 40);
    ASSERT(layout.overflow > 0);
    const auto* active = find_role(layout, Role::ActiveTab);
    ASSERT(active != nullptr);
    ASSERT(active->text.find("s5") != std::string::npos);
    bool saw_active_hit = false;
    for (const auto& hit : layout.hits)
        if (hit.id == 5)
            saw_active_hit = true;
    ASSERT(saw_active_hit);
}

TEST(top_bar_shrinks_titles_before_overflowing) {
    const Layout layout = tui::top_bar::layout(two_tabs(), 34);
    ASSERT_EQ(layout.overflow, 0);
    ASSERT(layout_width(layout) <= 34);
    const auto* active = find_role(layout, Role::ActiveTab);
    ASSERT(active != nullptr);
    ASSERT(active->text.find("\u2026") != std::string::npos);
}

TEST(top_bar_truncates_long_titles) {
    std::vector<Tab> tabs{{1, "a-very-long-session-title-that-must-not-push-the-clock", true, false,
                           tui::top_bar::Attention::None}};
    const Layout layout = tui::top_bar::layout(tabs, 80);
    ASSERT(layout_width(layout) <= 80);
    const auto* active = find_role(layout, Role::ActiveTab);
    ASSERT(active != nullptr);
    ASSERT(active->text.find("\u2026") != std::string::npos);
    ASSERT(tui::text::display_cols(active->text) <= 9 + 1 + tui::top_bar::kTabMaxTitle + 1);
}

TEST(top_bar_shows_running_and_attention_markers) {
    std::vector<Tab> tabs{
        {1, "running", false, true, tui::top_bar::Attention::None},
        {2, "needs-you", false, false, tui::top_bar::Attention::NeedsInput},
        {3, "idle", false, false, tui::top_bar::Attention::None},
    };
    const Layout layout = tui::top_bar::layout(tabs, 100);
    const auto* running = find_role(layout, Role::RunningTab);
    const auto* attention = find_role(layout, Role::AttentionTab);
    ASSERT(running != nullptr);
    ASSERT(attention != nullptr);
    ASSERT(running->text.find("\u25d0") != std::string::npos);
    ASSERT(attention->text.find("!") != std::string::npos);
}

TEST(top_bar_marks_done_and_error_tabs) {
    std::vector<Tab> tabs{
        {1, "done", false, false, tui::top_bar::Attention::Done},
        {2, "error", false, false, tui::top_bar::Attention::Error},
    };
    const Layout layout = tui::top_bar::layout(tabs, 100);
    const auto* done = find_role(layout, Role::DoneTab);
    const auto* error = find_role(layout, Role::ErrorTab);
    ASSERT(done != nullptr);
    ASSERT(error != nullptr);
    ASSERT(done->text.find("\u25cf") != std::string::npos);
    ASSERT(error->text.find("\u2717") != std::string::npos);
    ASSERT_EQ(tui::top_bar::attention_glyph(false, tui::top_bar::Attention::Done),
              std::string("\u25cf"));
    ASSERT_EQ(tui::top_bar::attention_glyph(false, tui::top_bar::Attention::Error),
              std::string("\u2717"));
    ASSERT_EQ(tui::top_bar::attention_glyph(false, tui::top_bar::Attention::Error, false),
              std::string("x"));
}

TEST(top_bar_with_no_tabs_is_just_the_app_mark) {
    const Layout layout = tui::top_bar::layout({}, 80);
    ASSERT_EQ(layout.spans.size(), 1u);
    ASSERT(layout.spans.front().role == Role::AppMark);
    ASSERT_EQ(layout.active_start, -1);
    ASSERT_EQ(layout.active_width, 0);
}

TEST(top_bar_never_paints_wider_than_its_budget) {
    for (int width = 0; width <= 120; ++width) {
        const Layout layout = tui::top_bar::layout(two_tabs(), width);
        ASSERT(layout_width(layout) <= width);
    }
}

TEST(top_bar_clock_is_minutes_only) {
    std::tm tm = local_tm(9, 5, 7);
    ASSERT_EQ(tui::top_bar::clock_text(std::mktime(&tm)), std::string("09:05"));

    std::tm midnight = local_tm(0, 0, 0);
    ASSERT_EQ(tui::top_bar::clock_text(std::mktime(&midnight)), std::string("00:00"));

    std::tm last = local_tm(23, 59, 59);
    ASSERT_EQ(tui::top_bar::clock_text(std::mktime(&last)), std::string("23:59"));
}

TEST(top_bar_pulse_alternates_once_a_second) {
    ASSERT_TRUE(tui::top_bar::pulse_on(0));
    ASSERT_FALSE(tui::top_bar::pulse_on(1));
    ASSERT_TRUE(tui::top_bar::pulse_on(2));
    ASSERT_EQ(tui::top_bar::pulse_glyph(true), std::string("\u25cf"));
    ASSERT_EQ(tui::top_bar::pulse_glyph(false), std::string("\u00b7"));
    ASSERT_EQ(tui::top_bar::pulse_glyph(true, false), std::string("*"));
    ASSERT_EQ(tui::top_bar::pulse_glyph(false, false), std::string("."));
}

TEST(top_bar_tab_width_matches_the_painted_label) {
    const std::vector<Tab> tabs{{1, "abc", true, false, tui::top_bar::Attention::None}};
    const Layout layout = tui::top_bar::layout(tabs, 80, true);
    const auto* active = find_role(layout, Role::ActiveTab);
    ASSERT(active != nullptr);
    ASSERT_EQ(tui::text::display_cols(active->text), tui::top_bar::tab_width(3, 1));
}
