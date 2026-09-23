#ifndef AMBER_TUI_SESSION_ROW_H
#define AMBER_TUI_SESSION_ROW_H

#include <cstddef>
#include <string>

// L1 (pure): the text and geometry the session browser's rows are built from.
// The ncurses shell (tui_session.cpp) only paints these.
namespace tui::session_row {

struct Size {
    int dw = 0;
    int dh = 0;
};
Size dialog_size(int screen_h, int screen_w);

// A title truncated to `title_w` columns, ellipsised when it does not fit.
std::string title(const std::string& t, int title_w);

// The model name truncated to 10 columns, ellipsised when it does not fit.
std::string model(const std::string& m);

// "N msgs".
std::string message_count(int n);

// Human size: "B", "KB" or "MB". Empty for 0 (the caller draws nothing).
std::string file_size(std::size_t bytes);

} // namespace tui::session_row

#endif // AMBER_TUI_SESSION_ROW_H
