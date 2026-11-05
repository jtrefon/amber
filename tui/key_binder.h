#ifndef AMBER_TUI_KEY_BINDER_H
#define AMBER_TUI_KEY_BINDER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/keybinding.h"
#include "tui/input_state.h"
#include "tui/key_read.h"

namespace tui {

// macOS terminals with default Option-key settings send the literal Option
// characters instead of an ESC prefix: Option+1 = U+00A1 '¡', Option+2 = U+2122
// '™', and so on. Map such a codepoint to its digit (0-9) so Option+digit
// switches windows with no terminal configuration. Returns -1 for codepoints
// outside the Option digit row.
int macos_option_digit(uint32_t codepoint);

// Option+B = U+222B '∫' on the same keyboards — the macOS spelling of
// Alt+B (delete word).
inline constexpr uint32_t kMacosOptionB = 0x222B;

// The closed vocabulary an ESC prefix may be built from: a window digit
// (1-9), word delete (b/B) or the mouse toggle (m/M). A key outside it is a
// key in its own right, so the input loop puts it back instead of letting the
// prefix read swallow it — a Ctrl+C queued behind ESC still quits.
bool esc_prefix_key(int ch) noexcept;

// Pure key-to-action mapping. v2 bindings are data: key -> action ID + scope +
// optional state predicate. The binder never dispatches; it returns the
// agent::KeyBinding whose action the host runs through the one ActionRegistry.
class KeyBinder {
public:
    explicit KeyBinder(nlohmann::json bindings, std::vector<agent::KeyBinding> extra = {});

    // Empty action means no binding for this key/state.
    agent::KeyBinding dispatch(const KeyRead& key, const InputState& state) const;

    // Core entries first; a plugin binding that repeats a key/scope/when is
    // refused and counted rather than silently replacing the core mapping.
    std::size_t collisions() const noexcept { return collisions_; }
    const std::vector<agent::KeyBinding>& bindings() const noexcept { return bindings_; }

private:
    void load(nlohmann::json bindings);
    void load_v2(const nlohmann::json& bindings);
    void load_v1(const nlohmann::json& bindings);
    static bool prepared(agent::KeyBinding& binding);
    void add(agent::KeyBinding binding);
    void add_legacy_defaults();
    agent::KeyBinding find(const std::string& key, const InputState& state) const;

    std::vector<agent::KeyBinding> bindings_;
    std::size_t collisions_ = 0;
};

} // namespace tui

#endif // AMBER_TUI_KEY_BINDER_H
