
#include "tui/session_browser_core.h"

namespace tui {

namespace {

// Case-insensitive substring match, preserving the browser's filter feel.
bool ci_contains(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return true;
    if (needle.size() > hay.size()) return false;
    auto lower = [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    for (size_t h = 0; h <= hay.size() - needle.size(); ++h) {
        size_t n = 0;
        while (n < needle.size() &&
               lower(hay[h + n]) == lower(needle[n]))
            ++n;
        if (n == needle.size()) return true;
    }
    return false;
}

} // namespace

BrowserLayout browser_layout(int dialog_rows) {
    BrowserLayout lay;
    int inner = dialog_rows - 2;          // rows between the border lines
    lay.list_h = inner > 1 ? inner - 1 : 1;
    lay.search_row = dialog_rows - 2;     // last inner row, below the list
    return lay;
}

SessionBrowserCore::SessionBrowserCore(std::vector<BrowserItem> items, int list_h)
    : items_(std::move(items)), list_h_(list_h) {
    rebuild();
    snap_sel();
    clamp_scroll();
}

void SessionBrowserCore::rebuild() {
    disp_.clear();
    long last_day = 0;
    bool first = true;
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        if (!ci_contains(items_[i].title, filter_)) continue;
        long day = items_[i].updated_ms / 86400000L;
        if (first || day != last_day) {
            disp_.emplace_back(0, i);
            last_day = day;
            first = false;
        }
        disp_.emplace_back(1, i);
    }
}

void SessionBrowserCore::snap_sel() {
    int n = static_cast<int>(disp_.size());
    if (n == 0) { sel_ = -1; return; }
    if (sel_ < 0) sel_ = 0;
    if (sel_ >= n) sel_ = n - 1;
    for (int step = 0; step < n; ++step) {
        if (sel_ + step < n && disp_[sel_ + step].first == 1) { sel_ += step; return; }
        if (sel_ - step >= 0 && disp_[sel_ - step].first == 1) { sel_ -= step; return; }
    }
    sel_ = -1;
}

void SessionBrowserCore::clamp_scroll() {
    if (scroll_off_ > sel_) scroll_off_ = sel_;
    if (scroll_off_ < 0) scroll_off_ = 0;
    if (sel_ >= scroll_off_ + list_h_) scroll_off_ = sel_ - list_h_ + 1;
}

SessionBrowserCore::Result SessionBrowserCore::key(int ch) {
    Result r;
    switch (ch) {
        case KEY_DOWN:
            if (sel_ >= 0) { ++sel_; snap_sel(); clamp_scroll(); }
            break;
        case KEY_UP:
            if (sel_ >= 0) { --sel_; snap_sel(); clamp_scroll(); }
            break;
        case KEY_NPAGE:
            if (sel_ >= 0) { sel_ += list_h_; snap_sel(); clamp_scroll(); }
            break;
        case KEY_PPAGE:
            if (sel_ >= 0) { sel_ -= list_h_; snap_sel(); clamp_scroll(); }
            break;
        case '\n': case '\r': case KEY_ENTER:
            r.closed = true;
            break;
        case KEY_DC: case 4:
            if (sel_ >= 0) r.delete_pending = true;
            break;
        case KEY_BACKSPACE: case 127: case 8:
            if (!filter_.empty()) filter_.pop_back();
            sel_ = 0;
            scroll_off_ = 0;
            rebuild();
            snap_sel();
            break;
        case 27:
            r.closed = true;
            break;
        default:
            if (ch >= 32 && ch <= 126) {
                filter_ += static_cast<char>(ch);
                sel_ = 0;
                scroll_off_ = 0;
                rebuild();
                snap_sel();
            }
            break;
    }
    return r;
}

int SessionBrowserCore::display_kind(int row) const {
    if (row < 0 || row >= display_count()) return -1;
    return disp_[row].first;
}

int SessionBrowserCore::display_item(int row) const {
    if (row < 0 || row >= display_count()) return -1;
    return disp_[row].second;
}

int SessionBrowserCore::load_index() const {
    if (sel_ < 0 || sel_ >= display_count()) return -1;
    return disp_[sel_].first == 1 ? disp_[sel_].second : -1;
}

void SessionBrowserCore::erase_current() {
    int idx = load_index();
    if (idx < 0) return;
    items_.erase(items_.begin() + idx);
    sel_ = 0;
    scroll_off_ = 0;
    rebuild();
    snap_sel();
}

} // namespace tui
