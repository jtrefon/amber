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

ListState::Action ListState::key(int ch, int max_visible) {
    if (ch == '/') {
        filter_mode_ = true;
        filter_.clear();
        selection_ = 0;
        scroll_offset_ = 0;
        return Action::Redraw;
    }

    if (filter_mode_) {
        if (ch == 27) { // Esc cancels the filter
            filter_mode_ = false;
            filter_.clear();
            selection_ = 0;
            scroll_offset_ = 0;
            return Action::Redraw;
        }
        if ((ch == keys::kBackspace || ch == 127) && !filter_.empty()) {
            filter_.pop_back();
            selection_ = 0;
            scroll_offset_ = 0;
            return Action::Redraw;
        }
        if (ch >= 32 && ch < 127) {
            filter_ += static_cast<char>(ch);
            selection_ = 0;
            scroll_offset_ = 0;
            return Action::Redraw;
        }
        // Enter still accepts the selection while filtering.
        if (ch == '\n' || ch == '\r') {
            filter_mode_ = false;
            return Action::Select;
        }
        return Action::None;
    }

    const std::vector<std::string> f = filtered();
    switch (ch) {
    case keys::kUp:
    case 'k':
        if (selection_ > 0) {
            --selection_;
            if (selection_ < scroll_offset_)
                scroll_offset_ = selection_;
            return Action::Redraw;
        }
        return Action::None;
    case keys::kDown:
    case 'j':
        if (selection_ < static_cast<int>(f.size()) - 1) {
            ++selection_;
            if (selection_ >= scroll_offset_ + max_visible)
                scroll_offset_ = selection_ - max_visible + 1;
            return Action::Redraw;
        }
        return Action::None;
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
