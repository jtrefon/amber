#ifndef AMBER_TUI_KEY_SOURCE_PORT_H
#define AMBER_TUI_KEY_SOURCE_PORT_H

#include "tui/key_read.h"

namespace tui {

// Port: terminal key input. The domain core uses this to read raw keys
// without depending on ncurses. The adapter (NcursesKeySource) implements
// getch/timeout and ESC followup reads.
class KeySourcePort {
public:
    virtual ~KeySourcePort() = default;
    virtual KeyRead read_key() = 0;
    virtual void set_timeout(int ms) = 0;
};

} // namespace tui

#endif // AMBER_TUI_KEY_SOURCE_PORT_H
