#include "tui/skills_browse_core.h"
#include "tui/keys.h"
#include "tests/test_util.h"

#include <string>
#include <vector>

namespace {

tui::SkillRow row(const std::string& name, int installs, const std::string& description = "",
                  const std::string& category = "", const std::string& source = "acme/skills") {
    tui::SkillRow r;
    r.ref.id = source + "/" + name;
    r.ref.name = name;
    r.ref.source = source;
    r.ref.installs = installs;
    r.description = description;
    r.category = category;
    return r;
}

std::vector<tui::SkillRow> sample() {
    return {row("tdd", 100, "Test-driven development", "engineering"),
            row("grill-me", 300, "Interview yourself", "career"),
            row("code-review", 200, "Review a branch", "engineering")};
}

} // namespace

TEST(skills_layout_breakpoints) {
    const auto wide = tui::skills_layout(120, 40);
    ASSERT_TRUE(wide.show_facets);
    ASSERT_TRUE(wide.show_detail);
    ASSERT_EQ(wide.facets_w, 24);
    ASSERT_EQ(wide.list_w, 40);
    ASSERT_EQ(wide.detail_w, 56);

    const auto medium = tui::skills_layout(80, 24);
    ASSERT_TRUE(!medium.show_facets);
    ASSERT_TRUE(medium.show_detail);
    ASSERT_EQ(medium.list_w, 34);
    ASSERT_EQ(medium.detail_w, 46);

    const auto narrow = tui::skills_layout(50, 20);
    ASSERT_TRUE(!narrow.show_facets);
    ASSERT_TRUE(!narrow.show_detail);
    ASSERT_EQ(narrow.list_w, 50);

    const auto shorty = tui::skills_layout(120, 8);
    ASSERT_TRUE(!shorty.show_detail);
    ASSERT_TRUE(!shorty.show_facets);
}

TEST(skills_browse_filter_matches_name_source_and_description) {
    tui::SkillsBrowseCore core;
    core.set_rows(sample());
    core.set_filter("tdd");
    ASSERT_EQ(static_cast<int>(core.visible_count()), 1);

    core.set_filter("code");
    ASSERT_EQ(static_cast<int>(core.visible_count()), 1);

    core.set_filter("engineering");
    ASSERT_EQ(static_cast<int>(core.visible_count()), 2);

    core.set_filter("ACME");
    ASSERT_EQ(static_cast<int>(core.visible_count()), 3);

    core.set_filter("nothing");
    ASSERT_EQ(static_cast<int>(core.visible_count()), 0);
}

TEST(skills_browse_sort_orders) {
    tui::SkillsBrowseCore core;
    core.set_rows(sample());
    core.set_sort(tui::SkillSort::Installs);
    ASSERT_EQ(core.visible()[0].ref.name, std::string("grill-me"));

    core.set_sort(tui::SkillSort::Name);
    ASSERT_EQ(core.visible()[0].ref.name, std::string("code-review"));

    core.set_sort(tui::SkillSort::Relevance);
    ASSERT_EQ(core.visible()[0].ref.name, std::string("tdd"));
}

TEST(skills_browse_selection_and_scroll) {
    tui::SkillsBrowseCore core;
    core.set_rows(sample());
    core.set_sort(tui::SkillSort::Relevance);
    ASSERT_EQ(core.selection(), 0);

    core.key('j', 2);
    core.key('j', 2);
    ASSERT_EQ(core.selection(), 2);
    ASSERT_EQ(core.scroll_offset(), 1);

    core.key('g', 2);
    ASSERT_EQ(core.selection(), 0);
    ASSERT_EQ(core.scroll_offset(), 0);

    core.key('G', 2);
    ASSERT_EQ(core.selection(), 2);
    ASSERT_EQ(core.scroll_offset(), 1);
}

TEST(skills_browse_search_pane_types_and_unwinds) {
    tui::SkillsBrowseCore core;
    core.set_rows(sample());
    core.key('/');
    ASSERT_TRUE(core.pane() == tui::SkillPane::Search);

    auto i = core.key('t');
    ASSERT_TRUE(i.kind == tui::SkillIntent::Kind::Search);
    i = core.key('d');
    ASSERT_TRUE(i.kind == tui::SkillIntent::Kind::Search);
    ASSERT_EQ(core.filter(), std::string("td"));
    ASSERT_EQ(static_cast<int>(core.visible_count()), 1);

    i = core.key(27); // Esc clears the filter
    ASSERT_TRUE(i.kind == tui::SkillIntent::Kind::Search);
    ASSERT_EQ(core.filter(), std::string(""));
    ASSERT_TRUE(core.pane() == tui::SkillPane::Search);

    core.key(27); // second Esc leaves the field
    ASSERT_TRUE(core.pane() == tui::SkillPane::List);
}

TEST(skills_browse_enter_opens_detail_and_esc_unwinds) {
    tui::SkillsBrowseCore core;
    core.set_rows(sample());
    core.set_sort(tui::SkillSort::Relevance);
    auto i = core.key('\n');
    ASSERT_TRUE(i.kind == tui::SkillIntent::Kind::OpenDetail);
    ASSERT_EQ(i.id, std::string("acme/skills/tdd"));
    ASSERT_TRUE(core.pane() == tui::SkillPane::Detail);

    core.key(27);
    ASSERT_TRUE(core.pane() == tui::SkillPane::List);
    i = core.key(27);
    ASSERT_TRUE(i.kind == tui::SkillIntent::Kind::Close);
}

TEST(skills_browse_install_and_marks) {
    tui::SkillsBrowseCore core;
    core.set_rows(sample());
    core.set_sort(tui::SkillSort::Relevance);

    auto i = core.key('i');
    ASSERT_TRUE(i.kind == tui::SkillIntent::Kind::Install);
    ASSERT_EQ(i.id, std::string("acme/skills/tdd"));

    core.key(' ');
    ASSERT_TRUE(core.marked("acme/skills/tdd"));
    core.key('j');
    core.key(' ');
    ASSERT_EQ(static_cast<int>(core.marks().size()), 2);

    i = core.key('i');
    ASSERT_TRUE(i.kind == tui::SkillIntent::Kind::InstallMarked);
    ASSERT_EQ(static_cast<int>(i.ids.size()), 2);

    core.key(' '); // unmark grill-me
    core.key('k'); // back to tdd
    core.key(' '); // unmark tdd
    i = core.key('i');
    ASSERT_TRUE(i.kind == tui::SkillIntent::Kind::Install);
}

TEST(skills_browse_mark_all_visible_ignores_filtered_out) {
    tui::SkillsBrowseCore core;
    core.set_rows(sample());
    core.set_filter("engineering");
    core.key('a');
    ASSERT_EQ(static_cast<int>(core.marks().size()), 2);
}

TEST(skills_browse_tabs_and_lifecycle_intents) {
    tui::SkillsBrowseCore core;
    core.set_rows(sample());
    auto i = core.key('1');
    ASSERT_TRUE(i.kind == tui::SkillIntent::Kind::SetTab);
    ASSERT_TRUE(i.tab == tui::SkillTab::ForYou);

    i = core.key('s');
    ASSERT_TRUE(i.kind == tui::SkillIntent::Kind::Sort);
    ASSERT_TRUE(i.sort == tui::SkillSort::Name);

    i = core.key('u');
    ASSERT_TRUE(i.kind == tui::SkillIntent::Kind::Update);
    i = core.key('U');
    ASSERT_TRUE(i.kind == tui::SkillIntent::Kind::UpdateAll);
    i = core.key('d');
    ASSERT_TRUE(i.kind == tui::SkillIntent::Kind::Diff);
    i = core.key('D');
    ASSERT_TRUE(i.kind == tui::SkillIntent::Kind::Remove);
    i = core.key('x');
    ASSERT_TRUE(i.kind == tui::SkillIntent::Kind::ToggleEnabled);
    i = core.key('A');
    ASSERT_TRUE(i.kind == tui::SkillIntent::Kind::Adopt);
    i = core.key('6');
    ASSERT_TRUE(i.kind == tui::SkillIntent::Kind::SetTab);
    ASSERT_TRUE(i.tab == tui::SkillTab::Installed);
    ASSERT_TRUE(core.tab() == tui::SkillTab::Installed);

    i = core.key('q');
    ASSERT_TRUE(i.kind == tui::SkillIntent::Kind::Close);
}

TEST(skills_browse_installed_tab_reads_the_installed_set) {
    tui::SkillsBrowseCore core;
    core.set_rows(sample());
    std::vector<tui::SkillRow> installed{row("mine", 0)};
    core.set_installed(std::move(installed));
    core.set_tab(tui::SkillTab::Installed);
    ASSERT_EQ(static_cast<int>(core.visible_count()), 1);
    ASSERT_EQ(core.visible()[0].ref.name, std::string("mine"));
}

TEST(skills_browse_empty_set_is_safe) {
    tui::SkillsBrowseCore core;
    core.set_rows({});
    ASSERT_TRUE(!core.current().has_value());
    ASSERT_EQ(static_cast<int>(core.visible_count()), 0);
    ASSERT_TRUE(core.key('j').kind == tui::SkillIntent::Kind::None);
    ASSERT_TRUE(core.key('\n').kind == tui::SkillIntent::Kind::None);
    ASSERT_TRUE(core.key('i').kind == tui::SkillIntent::Kind::None);
}

TEST(skills_browse_action_table_covers_row_actions) {
    tui::SkillsBrowseCore core;
    core.set_rows(sample());
    core.set_sort(tui::SkillSort::Relevance);

    struct Case {
        int ch;
        tui::SkillIntent::Kind kind;
    };
    const Case cases[] = {
        {'u', tui::SkillIntent::Kind::Update},        {'D', tui::SkillIntent::Kind::Remove},
        {'x', tui::SkillIntent::Kind::ToggleEnabled}, {'b', tui::SkillIntent::Kind::Block},
        {'A', tui::SkillIntent::Kind::Adopt},         {'d', tui::SkillIntent::Kind::Diff},
        {'p', tui::SkillIntent::Kind::Preview},       {'v', tui::SkillIntent::Kind::Audit},
        {'o', tui::SkillIntent::Kind::SourceUrl},     {'y', tui::SkillIntent::Kind::Copy},
    };
    for (const auto& c : cases) {
        const auto intent = core.key(c.ch);
        ASSERT_TRUE(intent.kind == c.kind);
        ASSERT_EQ(intent.id, std::string("acme/skills/tdd"));
    }
}

TEST(skills_browse_navigation_keys_are_handled) {
    tui::SkillsBrowseCore core;
    core.set_rows(sample());
    ASSERT_TRUE(core.key(tui::keys::kNPage, 2).kind == tui::SkillIntent::Kind::None);
    ASSERT_TRUE(core.key(tui::keys::kPPage, 2).kind == tui::SkillIntent::Kind::None);
    ASSERT_TRUE(core.key(4, 2).kind == tui::SkillIntent::Kind::None);
    ASSERT_TRUE(core.key(21, 2).kind == tui::SkillIntent::Kind::None);
    ASSERT_TRUE(core.key(tui::keys::kHome, 2).kind == tui::SkillIntent::Kind::None);
    ASSERT_TRUE(core.key(tui::keys::kEnd, 2).kind == tui::SkillIntent::Kind::None);
}

TEST(skills_browse_detail_scrolls_and_tab_cycles) {
    tui::SkillsBrowseCore core;
    core.set_rows(sample());
    core.key('\n');
    ASSERT_TRUE(core.pane() == tui::SkillPane::Detail);
    core.key('j');
    ASSERT_EQ(core.detail_scroll(), 1);
    core.key('k');
    ASSERT_EQ(core.detail_scroll(), 0);
    core.key('\t');
    ASSERT_TRUE(core.pane() == tui::SkillPane::List);
}

TEST(skills_updates_shelf_lists_only_skills_with_an_update) {
    tui::SkillsBrowseCore core;
    tui::SkillRow fresh = row("fresh", 1);
    fresh.installed = true;
    fresh.update = false;
    tui::SkillRow stale = row("stale", 2);
    stale.installed = true;
    stale.update = true;
    core.set_installed({fresh, stale});
    core.set_tab(tui::SkillTab::Updates);
    const auto visible = core.visible();
    ASSERT_EQ(visible.size(), static_cast<size_t>(1));
    ASSERT_EQ(visible[0].ref.name, std::string("stale"));
}

TEST(skills_shelf_availability_is_explicit) {
    ASSERT_TRUE(tui::skill_tab_available(tui::SkillTab::Top));
    ASSERT_TRUE(tui::skill_tab_available(tui::SkillTab::Installed));
    ASSERT_TRUE(tui::skill_tab_available(tui::SkillTab::Updates));
    ASSERT_FALSE(tui::skill_tab_available(tui::SkillTab::Trending));
    ASSERT_FALSE(tui::skill_tab_available(tui::SkillTab::Collections));
    ASSERT_EQ(std::string(tui::skill_tab_name(tui::SkillTab::ForYou)), std::string("For you"));
    ASSERT_EQ(std::string(tui::skill_tab_name(tui::SkillTab::Updates)), std::string("Updates"));
}

TEST(skills_categories_count_visible_rows) {
    tui::SkillsBrowseCore core;
    tui::SkillRow a = row("a", 1, "", "engineering");
    tui::SkillRow b = row("b", 2, "", "engineering");
    tui::SkillRow c = row("c", 3, "", "testing");
    tui::SkillRow none = row("d", 4);
    core.set_rows({a, b, c, none});
    const auto categories = core.categories();
    ASSERT_EQ(categories.size(), static_cast<size_t>(2));
    ASSERT_EQ(categories[0].first, std::string("engineering"));
    ASSERT_EQ(categories[0].second, 2);
    ASSERT_EQ(categories[1].first, std::string("testing"));
}
