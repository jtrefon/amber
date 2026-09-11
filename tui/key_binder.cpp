#include "tui/key_binder.h"

#include <utility>

namespace tui {

KeyBinder::KeyBinder(nlohmann::json bindings) : bindings_(std::move(bindings)) {}

KeyAction KeyBinder::dispatch(const KeyRead& key, const InputState& state) const {
    // Meta-encoded Alt+digit: 0xB1=Alt+1 through 0xB9=Alt+9.
    if (key.key >= 0xB1 && key.key <= 0xB9) {
        if (state.busy) return {};
        auto name = meta_digit_name(key.key);
        auto act = lookup_simple(name);
        if (act.type == KeyAction::SwitchWindow) return act;
        return {};
    }
    // ESC is stateful: drawer, Alt+digit followup, busy cancel, scroll toggle.
    if (key.key == 27) return dispatch_esc(key, state);
    // Ctrl+key mappings via JSON.
    if (key.key == 14) {  // Ctrl+N
        if (state.busy) return {};
        return lookup_simple("ctrl+n");
    }
    if (key.key == 3) return lookup_simple("ctrl+c");   // Ctrl+C
    if (key.key == 23) return lookup_simple("ctrl+w");  // Ctrl+W
    return {};
}

KeyAction KeyBinder::dispatch_esc(const KeyRead& key,
                                   const InputState& state) const {
    if (state.drawer_open) return {KeyAction::CloseDrawer, -1, ""};
    if (key.followup) {
        int f = *key.followup;
        if (f >= '1' && f <= '9') {
            if (state.busy) return {};
            return {KeyAction::SwitchWindow, f - '1', ""};
        }
        if (f == 'b' || f == 'B') return {KeyAction::DeleteWord, -1, ""};
    }
    if (state.busy) return {KeyAction::CancelOrQuit, -1, ""};
    return {KeyAction::ToggleScrollMode, -1, ""};
}

KeyAction KeyBinder::lookup_simple(const std::string& key_name) const {
    if (!bindings_.contains(key_name)) return {};
    const auto& entry = bindings_[key_name];
    if (!entry.contains("action")) return {};
    std::string action = entry["action"];
    int arg = entry.value("arg", -1);
    if (action == "switch_window")
        return {KeyAction::SwitchWindow, arg, ""};
    if (action == "new_window")
        return {KeyAction::NewWindow, -1, ""};
    if (action == "cancel_or_quit")
        return {KeyAction::CancelOrQuit, -1, ""};
    if (action == "delete_word")
        return {KeyAction::DeleteWord, -1, ""};
    return {};
}

std::string KeyBinder::meta_digit_name(int ch) {
    if (ch >= 0xB1 && ch <= 0xB9)
        return "alt+" + std::string(1, '1' + (ch - 0xB1));
    return {};
}

} // namespace tui
