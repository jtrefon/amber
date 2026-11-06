#ifndef AMBER_TUI_SKILLS_SCREEN_VIEW_H
#define AMBER_TUI_SKILLS_SCREEN_VIEW_H

// The skills explorer as a pure view-model plus painter. The explorer still owns
// the network, the lifecycle and the key intents; this file decides what the
// three panes look like and how the frame is built, so the screen can be tested
// at its breakpoints without a terminal.

#include "tui/screen_view.h"
#include "tui/skills_browse_core.h"
#include "tui/skills_detail_view.h"

#include <map>
#include <string>
#include <vector>

namespace tui {

struct SkillsFacetLine {
    std::string text;
    bool header = false;
    bool cursor = false;
};

struct SkillsListLine {
    std::string text;
    bool selected = false;
};

struct SkillsScreenView {
    SkillTab tab = SkillTab::Top;
    bool show_facets = false;
    bool show_detail = false;
    std::string search_field;
    std::string search_state;
    std::string sort_label;
    std::string budget;
    std::vector<SkillsFacetLine> facets;
    std::vector<SkillsListLine> rows;
    std::string empty_message;
    const DetailView* detail = nullptr;
    std::string detail_placeholder;
    int detail_scroll = 0;
    std::string footer;
};

struct SkillsScreenInput {
    std::string query;
    std::string footer;
    std::string budget;
    long long now_ms = 0;
};

// Build the screen from the (pure) browse core and the already-rendered detail
// bodies. `footer` is the caller's status line; the builder appends the
// list-only hint when the layout has no detail pane.
SkillsScreenView build_skills_screen(const SkillsBrowseCore& core, const SkillsScreenInput& input,
                                     const std::map<std::string, DetailView>& details, Size screen);

PaintedScreen paint_skills_screen(const SkillsScreenView& view, Size screen);

} // namespace tui

#endif // AMBER_TUI_SKILLS_SCREEN_VIEW_H
