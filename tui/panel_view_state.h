#ifndef AMBER_TUI_PANEL_VIEW_STATE_H
#define AMBER_TUI_PANEL_VIEW_STATE_H

#include <string>
#include <utility>
#include <vector>

// L1 (pure): scroll/geometry/footer model behind the contributed-panel view
// (panel_view.cpp). No ncurses — the view feeds keys and paints from these.
namespace tui::panel_view_state {

struct Scroll {
    int top = 0;
    int total = 0;
    int visible = 1;
    void clamp();
};

// The panel to start on: the requested id, else the first (the registry
// guarantees the first is the console).
int start_index(const std::vector<std::string>& ids, const std::string& start_id);

struct Geometry {
    int dh = 0;     // dialog height
    int dw = 0;     // dialog width
    int body_h = 0; // body window height
    int body_w = 0; // body window width
};
Geometry geometry(int screen_h, int screen_w);

// Footer key hints. Tab appears only when more than one panel exists.
std::vector<std::pair<std::string, std::string>> footer(int panel_count);

// The viewport rows, each truncated to `width` display columns.
std::vector<std::string> visible_rows(const std::vector<std::string>& lines, int width,
                                      const Scroll& scroll);

struct Arrows {
    bool up = false;
    bool down = false;
};
Arrows arrows(const Scroll& scroll);

// Apply a navigation key to the scroll position. Returns true when consumed;
// the position is not clamped here (clamp() runs before the next draw).
bool apply_key(int ch, Scroll& scroll);

} // namespace tui::panel_view_state

#endif // AMBER_TUI_PANEL_VIEW_STATE_H
