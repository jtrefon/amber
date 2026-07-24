// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AMBER_TUI_PANEL_H
#define AMBER_TUI_PANEL_H

#include <ncurses.h>
#include <panel.h>

#include "tui/widgets.h"

#include <memory>
#include <string>
#include <vector>

// Consistent ncurses panel with framed border, title, and footer.
// Follows ncurses best practices:
//   - ACS line drawing for borders (not ASCII characters)
//   - Title in top border: "──┤ Title ├───"
//   - Footer with keyboard shortcuts: "  [Tab] next  [Enter] select  [Esc] cancel  "
//   - Drop shadow via second panel
//   - Centered positioning by default

namespace tui {

// Color pair for panel elements
enum PanelPair {
    PP_BORDER = P_DIALOG,      // dialog body background
    PP_TITLE = P_BANNER,       // title text in border
    PP_FOOTER = P_STATUS,      // footer shortcut text
    PP_SHADOW = P_SHADOW,      // drop shadow
    PP_SELECT = P_BUTTON_ACT,  // selected item
    PP_ITEM = P_BUTTON,        // unselected item
};

struct FooterKey {
    std::string key;     // e.g. "Tab", "Enter", "Esc"
    std::string action;  // e.g. "next", "select", "cancel"
};

// Base class for all modal panels. Provides consistent framing,
// title, footer, shadow, and centered layout.
class Panel {
public:
    Panel(int h, int w, const std::string& title,
          std::vector<FooterKey> footer = {});
    virtual ~Panel();

    // Set footer keys after construction (used by Dialog compatibility).
    void set_footer(std::vector<FooterKey> footer) {
        footer_ = std::move(footer);
        if (win_) draw_frame();
    }

    Panel(const Panel&) = delete;
    Panel& operator=(const Panel&) = delete;

    // Access the content area (inside the border) for subclasses to draw in
    WINDOW* content() const { return content_win_; }
    int content_rows() const { return h_ - 2; }
    int content_cols() const { return w_ - 2; }

    // The outer window (with border). Used by legacy Dialog API for
    // callers that draw directly to the window surface.
    WINDOW* win() const { return win_; }
    int rows() const { return h_; }
    int cols() const { return w_; }

    // Top-left position (computed for centering)
    int top() const { return top_; }
    int left() const { return left_; }

    // Draw the border, title, and footer
    void draw_frame();

    // Show/hide the panel
    void show();
    void hide();

    // Process input. Return true if the panel handled it.
    virtual bool handle_key(int ch);

    // Show a help popup listing all available keys for this panel.
    virtual void show_help();

protected:
    int h_, w_;           // outer dimensions (including border)
    int top_, left_;      // centered position
    WINDOW* win_ = nullptr;
    WINDOW* content_win_ = nullptr;
    WINDOW* shadow_win_ = nullptr;
    PANEL* panel_ = nullptr;
    PANEL* shadow_panel_ = nullptr;
    std::string title_;
    std::vector<FooterKey> footer_;
};

} // namespace tui

#endif // AMBER_TUI_PANEL_H
