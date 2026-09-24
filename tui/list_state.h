#ifndef AMBER_TUI_LIST_STATE_H
#define AMBER_TUI_LIST_STATE_H

#include <string>
#include <vector>

// L1 (pure): the selection / filter / scroll model behind ListPanel. No ncurses
// — the panel feeds keys and paints from the accessors.
namespace tui {

class ListState {
public:
    // What the panel must do after a key: repaint, accept, or cancel.
    enum class Action { None, Redraw, Select, Cancel };

    explicit ListState(std::vector<std::string> items) : items_(std::move(items)) {}

    // Apply a key. `max_visible` is the panel's content height in rows.
    Action key(int ch, int max_visible);

    const std::vector<std::string>& items() const { return items_; }
    std::vector<std::string> filtered() const;
    const std::string& filter() const { return filter_; }
    bool filter_mode() const { return filter_mode_; }
    int selection() const { return selection_; }
    int scroll_offset() const { return scroll_offset_; }

    // The visible window [start, end) into filtered() for a viewport of
    // `max_visible` rows.
    void window(int max_visible, int& start, int& end) const;

    // Map the selected filtered index back to the original items_ index (no-op
    // when no filter is active).
    void remap_selection();

private:
    std::vector<std::string> items_;
    std::string filter_;
    int selection_ = 0;
    int scroll_offset_ = 0;
    bool filter_mode_ = false;
};

} // namespace tui

#endif // AMBER_TUI_LIST_STATE_H
