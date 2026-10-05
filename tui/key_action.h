#ifndef AMBER_TUI_KEY_ACTION_H
#define AMBER_TUI_KEY_ACTION_H

namespace tui {

// The typed intent produced by the KeyBinder from a raw key read. The
// InputLoop executes the action by delegating to the appropriate port or
// domain collaborator. This is the single dispatch type for all hotkeys.
//
// Every variant here has exactly one producer (a KeyBinder binding or a coded
// ESC path) and one consumer. That totality is enforced, not merely intended:
// tests/tui_tests.cpp drives each variant through the real binder, and Count
// below makes the test's table exhaustive-checkable. A variant with no binding
// is dead code that a compiler cannot report -- four of them (CloseWindow,
// Quit, Scroll, RouteToCommandLine) accumulated that way before the check
// existed.
//
// Reachability, not coverage, is the rule. A panel or console reachable by a
// key is expressed as an intent like any other, so the input loop never grows
// a branch per feature and new entry points stay additive.
struct KeyAction {
    enum Type {
        None,             // no binding for this key/state
        SwitchWindow,     // arg = zero-based window index
        NewWindow,        // switch to a freshly created window
        CancelOrQuit,     // Ctrl+C: cancel if busy, save+quit if idle
        ToggleScrollMode, // ESC in idle state
        CloseDrawer,      // ESC when drawer is open
        DeleteWord,       // Alt+B / Ctrl+W
        OpenPanels,       // Alt+0 / ESC+0: the panel view (registry console, status)
        // Sentinel, never produced and never acted on. Exists so a test can count the
        // variants above and fail when one is added without a binding and a case.
        Count,
    } type = None;
    int arg = -1; // window index, where a variant carries one
};

} // namespace tui

#endif // AMBER_TUI_KEY_ACTION_H
