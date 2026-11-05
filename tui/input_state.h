#ifndef AMBER_TUI_INPUT_STATE_H
#define AMBER_TUI_INPUT_STATE_H

#include <cstddef>
#include <string>

namespace tui {

// Immutable snapshot of the input-loop state at the moment a key is read.
// The KeyBinder uses this to make stateful dispatch decisions (e.g. ESC
// closes the drawer if open, cancels if busy, toggles scroll mode if idle).
struct InputState {
    bool drawer_open = false;
    bool busy = false;
    bool scroll_mode = false;
    size_t window_count = 0;
    bool has_pending_prompt = false;
    // The input surface the key arrived on: `input`, `menu`, `dialog` or
    // `list`. A global binding matches every surface; a scoped one only this.
    std::string scope = "input";
};

} // namespace tui

#endif // AMBER_TUI_INPUT_STATE_H
