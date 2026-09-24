#include "tui/session_row.h"

#include <algorithm>
#include <cstdio>

#include "tui/textutil.h"

namespace tui::session_row {

Size dialog_size(int screen_h, int screen_w) {
    Size s;
    s.dw = std::min(screen_w - 4, 120);
    s.dh = std::min(screen_h - 6, screen_h - 2);
    return s;
}

std::string title(const std::string& t, int title_w) {
    std::string out = t;
    if (static_cast<int>(out.size()) > title_w) {
        out.resize(static_cast<std::size_t>(title_w - 1));
        out += text::glyph::ellipsis();
    }
    return out;
}

std::string model(const std::string& m) {
    std::string out = m;
    if (out.size() > 10) {
        out.resize(9);
        out += text::glyph::ellipsis();
    }
    return out;
}

std::string message_count(int n) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%d msgs", n);
    return buf;
}

std::string file_size(std::size_t bytes) {
    if (bytes == 0)
        return "";
    char buf[16];
    if (bytes > static_cast<std::size_t>(1024) * 1024)
        std::snprintf(buf, sizeof(buf), "%.1fMB", bytes / (1024.0 * 1024.0));
    else if (bytes > 1024)
        std::snprintf(buf, sizeof(buf), "%.0fKB", bytes / 1024.0);
    else
        std::snprintf(buf, sizeof(buf), "%zuB", bytes);
    return buf;
}

} // namespace tui::session_row
