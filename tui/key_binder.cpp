#include "tui/key_binder.h"

namespace tui {

KeyBinder::KeyBinder(const nlohmann::json& bindings) : bindings_(bindings) {}

// RED stub: returns None for all inputs. Green implementation will map
// meta digits, ESC+digit, Ctrl+N, and stateful ESC to typed KeyActions.
KeyAction KeyBinder::dispatch(const KeyRead& key, const InputState& state) const {
    (void)key;
    (void)state;
    return {};
}

KeyAction KeyBinder::dispatch_esc(const KeyRead& key, const InputState& state) const {
    (void)key;
    (void)state;
    return {};
}

KeyAction KeyBinder::lookup_simple(const std::string& key_name) const {
    (void)key_name;
    return {};
}

std::string KeyBinder::meta_digit_name(int ch) {
    (void)ch;
    return {};
}

std::string KeyBinder::esc_digit_name(int followup) {
    (void)followup;
    return {};
}

} // namespace tui
