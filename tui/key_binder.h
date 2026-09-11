#ifndef AMBER_TUI_KEY_BINDER_H
#define AMBER_TUI_KEY_BINDER_H

#include <nlohmann/json.hpp>
#include "tui/input_state.h"
#include "tui/key_action.h"
#include "tui/key_read.h"

namespace tui {

// Pure key-to-action mapping. Translates raw terminal keys (meta-encoded
// digits, ESC+digit followups, Ctrl+keys) into typed KeyActions using a
// JSON-loaded binding table for simple mappings and coded logic for the
// stateful ESC key. Never performs I/O or side effects — fully testable.
class KeyBinder {
public:
    explicit KeyBinder(const nlohmann::json& bindings);
    KeyAction dispatch(const KeyRead& key, const InputState& state) const;

private:
    KeyAction dispatch_esc(const KeyRead& key, const InputState& state) const;
    KeyAction lookup_simple(const std::string& key_name) const;
    static std::string meta_digit_name(int ch);
    static std::string esc_digit_name(int followup);

    nlohmann::json bindings_;
};

} // namespace tui

#endif // AMBER_TUI_KEY_BINDER_H
