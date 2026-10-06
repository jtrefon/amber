#include "tui/list_state.h"

#include <algorithm>

#include "tui/keys.h"

namespace tui {

std::vector<std::string> ListState::filtered() const {
    if (filter_.empty())
        return items_;
    std::vector<std::string> out;
    for (const auto& item : items_)
        if (item.find(filter_) != std::string::npos)
            out.push_back(item);
    return out;
}

namespace {

bool is_backspace_key(int ch) {
    return ch == keys::kBackspace || ch == 127;
}

} // namespace

void ListState::reset_cursor() {
    selection_ = 0;
    scroll_offset_ = 0;
}

ListState::Action ListState::move_by(int delta, int max_visible, int count) {
    if (count <= 0)
        return Action::None;
    int sel = selection_ + delta;
    if (sel < 0)
        sel = 0;
    if (sel > count - 1)
        sel = count - 1;
    if (sel == selection_)
        return Action::None;
    selection_ = sel;
    if (max_visible > 0) {
        if (selection_ < scroll_offset_)
            scroll_offset_ = selection_;
        else if (selection_ >= scroll_offset_ + max_visible)
            scroll_offset_ = selection_ - max_visible + 1;
    }
    return Action::Redraw;
}

ListState::Action ListState::move_up(int max_visible) {
    return move_by(-1, max_visible, static_cast<int>(filtered().size()));
}

ListState::Action ListState::move_down(int max_visible, int count) {
    return move_by(1, max_visible, count);
}

// Arrow/page/home/end handling, shared by filter and normal mode so the two
// cannot drift. Typing "/" switches this widget straight into filter mode, and
// without navigation keys here the selection froze for as long as the filter
// stayed open -- KEY_UP/KEY_DOWN are 259/258, above the printable range, so
// they used to fall through the printable branch to Action::None and the
// arrows died the moment you typed to narrow a list.
ListState::Action ListState::key_nav(int ch, int max_visible) {
    const int count = static_cast<int>(filtered().size());
    const int page = std::max(1, max_visible);
    switch (ch) {
    case keys::kUp:
        return move_up(max_visible);
    case keys::kDown:
        return move_down(max_visible, count);
    case keys::kPPage:
        return move_by(-page, max_visible, count);
    case keys::kNPage:
        return move_by(page, max_visible, count);
    case keys::kHome:
        return move_by(-count, max_visible, count);
    case keys::kEnd:
        return move_by(count, max_visible, count);
    default:
        return Action::None;
    }
}

ListState::Action ListState::key_filter_mode(int ch, int max_visible) {
    const Action nav = key_nav(ch, max_visible);
    if (nav != Action::None)
        return nav;
    if (ch == 27) { // Esc cancels the filter
        filter_mode_ = false;
        filter_.clear();
        reset_cursor();
        return Action::Redraw;
    }
    if (is_backspace_key(ch) && !filter_.empty()) {
        filter_.pop_back();
        reset_cursor();
        return Action::Redraw;
    }
    if (ch >= 32 && ch < 127) {
        filter_ += static_cast<char>(ch);
        reset_cursor();
        return Action::Redraw;
    }
    // Enter still accepts the selection while filtering.
    if (ch == '\n' || ch == '\r') {
        filter_mode_ = false;
        return Action::Select;
    }
    return Action::None;
}

ListState::Action ListState::key_normal_mode(int ch, int max_visible) {
    const Action nav = key_nav(ch, max_visible);
    if (nav != Action::None)
        return nav;
    const int count = static_cast<int>(filtered().size());
    switch (ch) {
    case 'k':
        return move_up(max_visible);
    case 'j':
        return move_down(max_visible, count);
    case '\n':
    case '\r':
    case ' ':
        filter_mode_ = false;
        return Action::Select;
    case 27: // Esc
        filter_mode_ = false;
        selection_ = -1;
        return Action::Cancel;
    default:
        return Action::None;
    }
}

ListState::Action ListState::key(int ch, int max_visible) {
    if (ch == '/') {
        filter_mode_ = true;
        filter_.clear();
        reset_cursor();
        return Action::Redraw;
    }
    if (filter_mode_)
        return key_filter_mode(ch, max_visible);
    return key_normal_mode(ch, max_visible);
}

void ListState::window(int max_visible, int& start, int& end) const {
    const int n = static_cast<int>(filtered().size());
    start = std::min(scroll_offset_, std::max(0, n - max_visible));
    end = std::min(start + max_visible, n);
}

void ListState::remap_selection() {
    if (filter_.empty())
        return;
    const std::vector<std::string> f = filtered();
    if (selection_ >= 0 && selection_ < static_cast<int>(f.size())) {
        for (int i = 0; i < static_cast<int>(items_.size()); ++i)
            if (items_[i] == f[selection_]) {
                selection_ = i;
                break;
            }
    }
}

} // namespace tui
