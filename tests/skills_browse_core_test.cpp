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
    i = core.key('?');
    ASSERT_TRUE(i.kind == tui::SkillIntent::Kind::Help);
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

TEST(skills_batch_plan_skips_collisions_and_unread_sources) {
    std::vector<tui::BatchCandidate> candidates(4);
    candidates[0].id = "acme/skills/clean";
    candidates[0].name = "clean";
    candidates[0].previewed = true;
    candidates[1].id = "acme/skills/unread";
    candidates[1].name = "unread";
    candidates[2].id = "acme/skills/official";
    candidates[2].name = "official";
    candidates[2].official = true;
    candidates[3].id = "acme/skills/taken";
    candidates[3].name = "taken";
    candidates[3].previewed = true;
    candidates[3].exists_in_scope = true;

    const tui::BatchPlan plan = tui::plan_batch(candidates, true);
    ASSERT_EQ(plan.install_ids.size(), static_cast<size_t>(2));
    ASSERT_EQ(plan.install_ids[0], std::string("acme/skills/clean"));
    ASSERT_EQ(plan.install_ids[1], std::string("acme/skills/official"));
    ASSERT_EQ(plan.skipped.size(), static_cast<size_t>(2));
    ASSERT_EQ(plan.skipped[0], std::string("unread (review required: p reads it)"));
    ASSERT_EQ(plan.skipped[1], std::string("taken (name collision)"));
}

TEST(skills_batch_plan_with_review_off_installs_everything_new) {
    std::vector<tui::BatchCandidate> candidates(2);
    candidates[0].id = "acme/skills/a";
    candidates[0].name = "a";
    candidates[1].id = "acme/skills/b";
    candidates[1].name = "b";
    const tui::BatchPlan plan = tui::plan_batch(candidates, false);
    ASSERT_EQ(plan.install_ids.size(), static_cast<size_t>(2));
    ASSERT_TRUE(plan.skipped.empty());
}

namespace {

tui::SkillRow facet_row(const std::string& name, const std::string& category, agent::TrustTier tier,
                        bool installed, const std::string& state = "enabled",
                        const std::string& source = "acme/skills") {
    tui::SkillRow r = row(name, 1);
    r.ref.source = source;
    r.category = category;
    r.tier = tier;
    r.installed = installed;
    r.state = installed ? state : std::string();
    return r;
}

} // namespace

TEST(skills_facets_are_or_within_a_group_and_across_groups) {
    tui::SkillsBrowseCore core;
    core.set_rows({facet_row("a", "testing", agent::TrustTier::Community, false),
                   facet_row("b", "testing", agent::TrustTier::Official, false),
                   facet_row("c", "docs", agent::TrustTier::Community, false)});

    core.toggle_facet(tui::FacetGroup::Category, "testing");
    ASSERT_EQ(core.visible_count(), static_cast<size_t>(2));

    // OR within the group: adding a second category widens the set.
    core.toggle_facet(tui::FacetGroup::Category, "docs");
    ASSERT_EQ(core.visible_count(), static_cast<size_t>(3));

    // AND across groups: trust narrows what category allowed.
    core.toggle_facet(tui::FacetGroup::Trust, "official");
    ASSERT_EQ(core.visible_count(), static_cast<size_t>(1));
    ASSERT_EQ(core.visible()[0].ref.name, std::string("b"));

    core.clear_facets();
    ASSERT_FALSE(core.any_facet());
    ASSERT_EQ(core.visible_count(), static_cast<size_t>(3));
}

TEST(skills_facet_counts_ignore_their_own_selection) {
    tui::SkillsBrowseCore core;
    core.set_rows({facet_row("a", "testing", agent::TrustTier::Community, false),
                   facet_row("b", "testing", agent::TrustTier::Official, false),
                   facet_row("c", "docs", agent::TrustTier::Community, false)});
    core.toggle_facet(tui::FacetGroup::Category, "testing");

    // The category counts still describe the whole loaded set, so a selected
    // value keeps a truthful count and the others remain reachable.
    const auto counts = core.facet_counts(tui::FacetGroup::Category);
    ASSERT_EQ(counts.size(), static_cast<size_t>(2));
    ASSERT_EQ(counts[0].first, std::string("testing"));
    ASSERT_EQ(counts[0].second, 2);
    ASSERT_EQ(counts[1].first, std::string("docs"));
    ASSERT_EQ(counts[1].second, 1);

    // Trust counts compose with the category filter.
    const auto trust = core.facet_counts(tui::FacetGroup::Trust);
    ASSERT_EQ(trust.size(), static_cast<size_t>(2));
    ASSERT_EQ(trust[0].first, std::string("community"));
    ASSERT_EQ(trust[0].second, 1);
}

TEST(skills_facets_bucket_uncategorised_and_status_visibly) {
    tui::SkillRow plain = facet_row("plain", "", agent::TrustTier::Unverified, false);
    tui::SkillRow off = facet_row("off", "testing", agent::TrustTier::Community, true, "disabled");
    tui::SkillsBrowseCore core;
    core.set_rows({plain});
    core.set_installed({off});

    ASSERT_EQ(tui::facet_value(plain, tui::FacetGroup::Category), std::string("uncategorised"));
    ASSERT_EQ(tui::facet_value(off, tui::FacetGroup::Status), std::string("disabled"));
    ASSERT_EQ(tui::facet_value(plain, tui::FacetGroup::Status), std::string("not installed"));
    ASSERT_EQ(tui::facet_value(off, tui::FacetGroup::Trust), std::string("community"));
}

TEST(skills_filter_banner_names_a_live_query) {
    tui::SkillsBrowseCore core;
    ASSERT_TRUE(core.filter_banner().empty());
    core.set_rows(sample());
    core.set_query_live(true);
    ASSERT_TRUE(core.filter_banner().find("live query") != std::string::npos);
    core.toggle_facet(tui::FacetGroup::Category, "testing");
    ASSERT_TRUE(core.filter_banner().find("loaded results") != std::string::npos);
}

TEST(skills_facet_and_clear_keys_are_intents) {
    tui::SkillsBrowseCore core;
    core.set_rows(sample());
    auto i = core.key('f');
    ASSERT_TRUE(i.kind == tui::SkillIntent::Kind::FocusFacets);
    i = core.key('r');
    ASSERT_TRUE(i.kind == tui::SkillIntent::Kind::ClearFacets);
}

TEST(skills_facet_pane_walks_groups_and_toggles_values) {
    tui::SkillsBrowseCore core;
    core.set_rows({facet_row("a", "testing", agent::TrustTier::Community, false),
                   facet_row("b", "docs", agent::TrustTier::Official, false)});

    core.key('f');
    ASSERT_TRUE(core.pane() == tui::SkillPane::Facets);
    ASSERT_EQ(tui::facet_group_name(core.facet_cursor().group), std::string("status"));
    ASSERT_EQ(core.facet_cursor_value(), std::string("not installed"));

    // Toggling the value under the cursor is a state change plus an intent, so
    // the renderer can repaint and the host can persist.
    const auto toggled = core.key('\r');
    ASSERT_TRUE(toggled.kind == tui::SkillIntent::Kind::ToggleFacet);
    ASSERT_TRUE(core.any_facet());
    ASSERT_TRUE(core.facet_selected(tui::FacetGroup::Status, "not installed"));
    ASSERT_EQ(core.visible_count(), static_cast<size_t>(2)); // both are not installed

    // j walks to the next value, or wraps into the next group.
    core.key('j');
    ASSERT_EQ(tui::facet_group_name(core.facet_cursor().group), std::string("trust"));
    ASSERT_EQ(core.facet_cursor_value(), std::string("community"));

    core.key('r');
    ASSERT_FALSE(core.any_facet());
    ASSERT_TRUE(core.pane() == tui::SkillPane::Facets); // reset keeps the pane

    core.key(27);
    ASSERT_TRUE(core.pane() == tui::SkillPane::List);
}

TEST(skills_facet_pane_leaves_on_tab_switch) {
    tui::SkillsBrowseCore core;
    core.set_rows({facet_row("a", "testing", agent::TrustTier::Community, false)});
    core.key('f');
    ASSERT_TRUE(core.pane() == tui::SkillPane::Facets);
    core.set_tab(tui::SkillTab::Installed);
    ASSERT_TRUE(core.pane() == tui::SkillPane::List);
}

TEST(skills_batch_report_names_all_three_outcomes) {
    ASSERT_EQ(tui::batch_report(2, {"taken (name collision)"}, 1, {"broken (network)"}, false),
              std::string("2 installed, 1 skipped, 1 failed: taken (name collision), "
                          "broken (network)"));
    ASSERT_EQ(tui::batch_report(0, {"unread (review required: p reads it)"}, 0, {}, false),
              std::string("0 installed, 1 skipped: unread (review required: p reads it)"));
    ASSERT_EQ(tui::batch_report(1, {}, 0, {}, true),
              std::string("1 installed · cancelled, the rest was not started"));
}

TEST(skills_marks_survive_a_filter_change) {
    tui::SkillsBrowseCore core;
    core.set_rows(sample());
    core.key(' ');
    const std::string marked = core.current()->ref.id;
    ASSERT_TRUE(core.marked(marked));
    core.set_filter("grill");
    ASSERT_TRUE(core.marked(marked)); // keyed by id, not by row index
    core.set_rows(sample());          // a refresh keeps it too
    ASSERT_TRUE(core.marked(marked));
    core.clear_marks();
    ASSERT_TRUE(core.marks().empty());
}
