// The skills screen is three panes over one cell grid. These tests pin its
// breakpoints, its search/tab/status line, the list and facet rows, the detail
// pane and the install sheet's frame, without a terminal.

#include "test_util.h"
#include "tui/skills_install_sheet.h"
#include "tui/skills_screen_view.h"
#include "tui/skills_sheet_view.h"

#include <string>
#include <vector>

namespace {

tui::SkillRow make_row() {
    tui::SkillRow row;
    row.ref.id = "acme/skills/tdd";
    row.ref.name = "tdd";
    row.ref.source = "acme/skills";
    row.ref.installs = 12345;
    row.category = "engineering";
    row.state = "enabled";
    row.installed = true;
    row.official = true;
    row.audit = agent::AuditVerdict::Pass;
    row.audited = true;
    return row;
}

tui::DetailView make_detail() {
    tui::DetailView detail;
    detail.id = "acme/skills/tdd";
    detail.name = "tdd";
    detail.source = "acme/skills";
    detail.headline = "Test-driven development.";
    detail.evidence = {"Community source", "2 files"};
    detail.audit = "pass";
    detail.body = {tui::rich::Line{{tui::rich::Run{"# TDD", tui::P_MD_HEAD, true}}}};
    return detail;
}

bool row_contains(const tui::Surface& surface, int row, const std::string& needle) {
    return surface.row_text(row).find(needle) != std::string::npos;
}

} // namespace

TEST(skills_screen_wide_layout_paints_facets_list_and_detail) {
    tui::SkillsBrowseCore core;
    core.set_rows({make_row()});
    const std::map<std::string, tui::DetailView> details{{"acme/skills/tdd", make_detail()}};

    const tui::SkillsScreenView view =
        tui::build_skills_screen(core, {"tdd", "loaded tdd", "", 1000}, details, {120, 40});
    ASSERT(view.show_facets);
    ASSERT(view.show_detail);
    ASSERT_EQ(view.rows.size(), 1u);
    ASSERT(view.rows[0].selected);
    ASSERT(view.detail != nullptr);
    ASSERT(view.footer == std::string("loaded tdd"));
    ASSERT(!view.facets.empty());

    const tui::PaintedScreen painted = tui::paint_skills_screen(view, {120, 40});
    ASSERT(row_contains(painted.surface, 0, "/ tdd"));
    ASSERT(row_contains(painted.surface, 0, "sort: installs"));
    ASSERT(row_contains(painted.surface, 1, "1 For you"));
    ASSERT(row_contains(painted.surface, 1, "3 Top"));
    ASSERT(row_contains(painted.surface, 3, "Filters"));
    ASSERT(row_contains(painted.surface, 3, "tdd"));
    ASSERT(row_contains(painted.surface, 3, "12K"));
    ASSERT(row_contains(painted.surface, 39, "loaded tdd"));

    // The detail pane starts after the facet column (24) and the list (40+2).
    bool detail = false;
    for (int y = 3; y < 39; ++y)
        detail = detail ||
                 painted.surface.row_text(y).find("Test-driven development.") != std::string::npos;
    ASSERT(detail);
}

TEST(skills_screen_medium_layout_drops_only_the_facet_column) {
    tui::SkillsBrowseCore core;
    core.set_rows({make_row()});
    const std::map<std::string, tui::DetailView> details{{"acme/skills/tdd", make_detail()}};

    const tui::SkillsScreenView view =
        tui::build_skills_screen(core, {"", "", "14 lines", 1000}, details, {80, 24});
    ASSERT(!view.show_facets);
    ASSERT(view.show_detail);
    ASSERT(view.detail != nullptr);
    ASSERT_EQ(view.search_field, std::string("type to search (2+ chars)"));

    const tui::PaintedScreen painted = tui::paint_skills_screen(view, {80, 24});
    ASSERT(row_contains(painted.surface, 3, "tdd"));
    bool detail = false;
    for (int y = 3; y < 23; ++y)
        detail = detail ||
                 painted.surface.row_text(y).find("Test-driven development.") != std::string::npos;
    ASSERT(detail);
}

TEST(skills_screen_narrow_layout_is_list_only_and_explains_why) {
    tui::SkillsBrowseCore core;
    core.set_rows({make_row()});
    const std::map<std::string, tui::DetailView> details{{"acme/skills/tdd", make_detail()}};

    const tui::SkillsScreenView view =
        tui::build_skills_screen(core, {"", "", "", 1000}, details, {58, 17});
    ASSERT(!view.show_detail);
    ASSERT(view.footer.find("list-only, 60x18") != std::string::npos);

    const tui::PaintedScreen painted = tui::paint_skills_screen(view, {58, 17});
    ASSERT(row_contains(painted.surface, 0, "type to search"));
    ASSERT(row_contains(painted.surface, 3, "tdd"));
    ASSERT(row_contains(painted.surface, 16, "list-only, 60x18"));
}

TEST(skills_screen_empty_shelf_names_the_reason) {
    tui::SkillsBrowseCore core;
    const tui::SkillsScreenView installed =
        tui::build_skills_screen(core, {"", "", "", 1000}, {}, {80, 24});
    // The builder reads the core's tab; a fresh core is Top, which asks for a
    // query rather than claiming the shelf is empty.
    ASSERT_EQ(installed.empty_message, std::string("Type a query to search."));

    core.set_tab(tui::SkillTab::Updates);
    const tui::SkillsScreenView updates =
        tui::build_skills_screen(core, {"", "", "", 1000}, {}, {80, 24});
    ASSERT_EQ(updates.empty_message, std::string("No updates. Press 6, then u to check."));
}

TEST(skills_detail_pane_paints_header_and_markdown_body) {
    const tui::DetailView detail = make_detail();
    tui::Surface surface(42, 16);
    surface.clear(tui::Cell{" ", tui::Style{tui::Role::Surface}});
    tui::paint_detail_pane(surface, tui::DetailRect{1, 2, 13, 38}, detail, 0);

    ASSERT(row_contains(surface, 1, "tdd"));
    ASSERT(row_contains(surface, 2, "acme/skills"));
    ASSERT(row_contains(surface, 3, "Test-driven development."));
    ASSERT(row_contains(surface, 5, "Community source"));
    ASSERT(row_contains(surface, 7, "Audit"));
    ASSERT(row_contains(surface, 9, "SKILL.md preview"));
    ASSERT(row_contains(surface, 10, "TDD"));
    bool heading = false;
    for (int x = 2; x < 40; ++x)
        heading = heading || (surface.at(x, 10).glyph == std::string("T") &&
                              surface.at(x, 10).style.role == tui::Role::Attention);
    ASSERT(heading);
}

TEST(install_sheet_frame_shows_scope_evidence_and_the_gate) {
    tui::InstallEvidence evidence;
    evidence.name = "tdd";
    evidence.source = "acme/skills";
    evidence.category = "engineering";
    evidence.license = "MIT";
    evidence.files = 2;
    evidence.bytes = 2048;
    evidence.tokens = 400;
    evidence.tier = agent::TrustTier::Community;

    tui::InstallSheetOptions options;
    tui::InstallSheet sheet(evidence, options);
    const tui::PaintedDialog painted = tui::paint_install_sheet(sheet, {80, 24});
    ASSERT(painted.surface.width() >= 40);
    ASSERT(painted.surface.at(0, 0).glyph == std::string("\u250c") ||
           painted.surface.at(0, 0).glyph == std::string("+"));

    bool title = false;
    bool scope = false;
    bool evidence_row = false;
    bool footer = false;
    for (int y = 0; y < painted.surface.height(); ++y) {
        const std::string row = painted.surface.row_text(y);
        title = title || row.find("Install skill") != std::string::npos;
        scope = scope || row.find("Scope") != std::string::npos;
        evidence_row = evidence_row || row.find("Evidence") != std::string::npos;
        footer = footer || row.find("Enter: install") != std::string::npos;
    }
    ASSERT(title);
    ASSERT(scope);
    ASSERT(evidence_row);
    ASSERT(footer);
    ASSERT(painted.x >= 0 && painted.y >= 0);
    ASSERT(painted.x + painted.surface.width() <= 80);
    ASSERT(painted.y + painted.surface.height() <= 24);
}

TEST(install_sheet_frame_follows_scope_enable_and_the_gate) {
    tui::InstallEvidence evidence;
    evidence.name = "tdd";
    evidence.source = "acme/skills";
    evidence.files = 2;
    evidence.bytes = 2048;
    evidence.tokens = 400;

    tui::InstallSheetOptions options;
    tui::InstallSheet sheet(evidence, options);
    ASSERT(sheet.key('\t') == tui::InstallSheetAction::ToggleScope);
    ASSERT(sheet.key('e') == tui::InstallSheetAction::ToggleEnabled);

    const tui::PaintedDialog painted = tui::paint_install_sheet(sheet, {80, 24});
    bool global = false;
    bool disabled = false;
    bool review = false;
    for (int y = 0; y < painted.surface.height(); ++y) {
        const std::string row = painted.surface.row_text(y);
        global = global || row.find("(o) global") != std::string::npos;
        disabled = disabled || row.find("(o) install disabled") != std::string::npos;
        review = review || row.find("review required") != std::string::npos;
    }
    ASSERT(global);
    ASSERT(disabled);
    ASSERT(review);
}
