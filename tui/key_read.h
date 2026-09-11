#ifndef AMBER_TUI_KEY_READ_H
#define AMBER_TUI_KEY_READ_H

#include <optional>

namespace tui {

// Raw key read from the terminal. When the primary key is ESC (27), the
// followup carries the next key if one arrived within the ESC timeout window
// (used to disambiguate Alt+digit from a lone Escape).
struct KeyRead {
    int key = 0;
    std::optional<int> followup;
};

} // namespace tui

#endif // AMBER_TUI_KEY_READ_H
