
#ifndef AMBER_TUI_SESSION_BROWSER_CORE_H
#define AMBER_TUI_SESSION_BROWSER_CORE_H

#include <ncurses.h>

#include <string>
#include <utility>
#include <vector>

namespace tui {

// One saved session as shown in the browser.
struct BrowserItem {
    std::string id;
    std::string title;
    long updated_ms = 0;
};

// Dialog geometry: the list fills the inner area above the search bar,
// which owns the last inner row (never overlaps list rows).
struct BrowserLayout {
    int list_h;
    int search_row;
};
BrowserLayout browser_layout(int dialog_rows);

// Pure decision core of the session browser: selection over interleaved
// date-header/session rows, filtering, scrolling, key semantics. No
// ncurses calls — the renderer feeds keys and paints from accessors.
class SessionBrowserCore {
public:
    SessionBrowserCore(std::vector<BrowserItem> items, int list_h);

    struct Result {
        bool closed = false;
        bool delete_pending = false;
    };

    Result key(int ch);

    int sel() const { return sel_; }
    int scroll_off() const { return scroll_off_; }
    int list_h() const { return list_h_; }
    std::string filter() const { return filter_; }
    int display_count() const { return static_cast<int>(disp_.size()); }

    // Row kind: 0 = date header, 1 = session, -1 = out of range.
    int display_kind(int row) const;
    // Items index behind a session row (-1 for headers/out of range).
    int display_item(int row) const;

    // Index into items_ the current selection resolves to (-1 if none).
    int load_index() const;

    // Remove the currently selected session (after confirmation) and
    // reset navigation to the top.
    void erase_current();

private:
    void rebuild();
    void snap_sel();
    void clamp_scroll();

    std::vector<BrowserItem> items_;
    std::vector<std::pair<int, int>> disp_;   // (kind, items index)
    std::string filter_;
    int sel_ = 0;
    int scroll_off_ = 0;
    int list_h_;
};

} // namespace tui

#endif // AMBER_TUI_SESSION_BROWSER_CORE_H
