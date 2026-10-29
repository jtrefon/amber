#ifndef AMBER_TUI_SKILLS_DETAIL_VIEW_H
#define AMBER_TUI_SKILLS_DETAIL_VIEW_H

#include "agent/skill_index.h"
#include "tui/rich.h"
#include "tui/skills_browse_core.h"

#include <map>
#include <string>
#include <vector>

#include <ncurses.h>

namespace tui {

// Everything the detail pane paints for one skill, built once when the detail
// (or a second file) arrives so a repaint never re-renders Markdown.
struct DetailView {
    std::string id;
    std::string name;
    std::string source;
    std::string headline;
    std::vector<std::string> evidence;
    std::string audit;
    std::string cost;
    std::vector<agent::SkillAsset> assets;
    std::size_t file = 0; // index into `assets`
    std::vector<rich::Line> body;
    std::string note; // "loading files..." or why the file is not previewed
};

// `files` holds the bytes of any file already fetched, keyed by path; the
// SKILL.md body always comes from `detail`. A file with no bytes renders a
// note instead of an empty pane, and a binary is never rendered as text.
DetailView build_detail_view(const agent::SkillDetail& detail, const DetailInstall& install,
                             int body_budget_tokens,
                             const std::map<std::string, std::string>* files,
                             std::size_t file_index);

// The pane's rectangle in the dialog window.
struct DetailRect {
    int top = 0;
    int left = 0;
    int height = 0;
    int width = 0;
};

// Paint the pane. `scroll` is the first body line shown; the renderer clamps it,
// so a stale scroll cannot paint past the body.
void draw_detail_pane(WINDOW* w, const DetailRect& rect, const DetailView& view, int scroll);

} // namespace tui

#endif // AMBER_TUI_SKILLS_DETAIL_VIEW_H
