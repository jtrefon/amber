#ifndef AMBER_TUI_KEY_ACTION_H
#define AMBER_TUI_KEY_ACTION_H

#include <string>

namespace tui {

// The typed intent produced by the KeyBinder from a raw key read. The
// InputLoop executes the action by delegating to the appropriate port or
// domain collaborator. This is the single dispatch type for all hotkeys.
struct KeyAction {
    enum Type {
        None,               // no binding for this key/state
        SwitchWindow,       // arg = zero-based window index
        NewWindow,          // switch to a freshly created window
        CloseWindow,        // close the active window
        CancelOrQuit,       // Ctrl+C: cancel if busy, save+quit if idle
        Quit,               // unconditional quit
        ToggleScrollMode,   // ESC in idle state
        CloseDrawer,        // ESC when drawer is open
        DeleteWord,         // Alt+B / Ctrl+W
        Scroll,             // arg = scroll delta (lines)
        RouteToCommandLine, // arg = raw key code, hand off to CommandLine
    } type = None;
    int arg = -1;           // window index, scroll delta, or key code
    std::string text;       // optional text payload (unused for keys)
};

} // namespace tui

#endif // AMBER_TUI_KEY_ACTION_H
