#include "tui/key_binder.h"

#include <utility>

namespace tui {

namespace {

std::string lowercase(std::string text) {
    for (char& c : text)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return text;
}

bool when_matches(const std::string& when, const InputState& state) {
    if (when == "always")
        return true;
    if (when == "drawer")
        return state.drawer_open;
    if (when == "busy")
        return state.busy;
    if (when == "idle")
        return !state.drawer_open && !state.busy;
    return false;
}

} // namespace

int macos_option_digit(uint32_t codepoint) {
    switch (codepoint) {
    case 0x00A1:
        return 1; // ¡
    case 0x2122:
        return 2; // ™
    case 0x00A3:
        return 3; // £
    case 0x00A2:
        return 4; // ¢
    case 0x221E:
        return 5; // ∞
    case 0x00A6:
        return 6; // §
    case 0x00B6:
        return 7; // ¶
    case 0x2022:
        return 8; // •
    case 0x00AA:
        return 9; // ª
    case 0x00BA:
        return 0; // º
    default:
        return -1;
    }
}

bool esc_prefix_key(int ch) noexcept {
    return (ch >= '1' && ch <= '9') || ch == 'b' || ch == 'B' || ch == 'm' || ch == 'M';
}

KeyBinder::KeyBinder(nlohmann::json bindings, std::vector<agent::KeyBinding> extra) {
    load(std::move(bindings));
    for (auto& binding : extra)
        add(std::move(binding));
}

void KeyBinder::add(agent::KeyBinding binding) {
    if (binding.key.empty() || binding.action.empty())
        return;
    if (binding.scope.empty())
        binding.scope = "global";
    if (binding.when.empty())
        binding.when = "always";
    for (const auto& existing : bindings_) {
        if (existing.key == binding.key && existing.scope == binding.scope &&
            existing.when == binding.when) {
            ++collisions_;
            return;
        }
    }
    bindings_.push_back(std::move(binding));
}

void KeyBinder::load(nlohmann::json bindings) {
    if (!bindings.is_object())
        return;
    if (bindings.contains("bindings") && bindings["bindings"].is_array()) {
        load_v2(bindings);
        return;
    }
    load_v1(bindings);
}

void KeyBinder::load_v2(const nlohmann::json& bindings) {
    for (const auto& entry : bindings["bindings"]) {
        if (!entry.is_object())
            continue;
        agent::KeyBinding binding;
        binding.key = entry.value("key", std::string());
        binding.action = entry.value("action", std::string());
        binding.arg = entry.value("arg", std::string());
        binding.scope = entry.value("scope", std::string("global"));
        binding.when = entry.value("when", std::string("always"));
        add(std::move(binding));
    }
}

// v1 was a map of internal intents rather than action IDs. Translate the ones
// the coded binder understood; anything else is skipped rather than invented.
// ESC handling was coded, never table-driven, so those entries are ignored and
// the same defaults the v1 host applied are added below.
void KeyBinder::load_v1(const nlohmann::json& bindings) {
    for (auto it = bindings.begin(); it != bindings.end(); ++it) {
        if (!it.value().is_object() || it.key().rfind("esc", 0) == 0)
            continue;
        const std::string action = it.value().value("action", std::string());
        const long arg = it.value().value("arg", 0L);
        if (action == "open_panels") {
            add(agent::KeyBinding{it.key(), "core.panel", "", "global", "always", {}});
        } else if (action == "switch_window") {
            add(agent::KeyBinding{
                it.key(), "core.window.set", std::to_string(arg + 1), "global", "always", {}});
        } else if (action == "new_window") {
            add(agent::KeyBinding{it.key(), "core.window.new", "", "global", "always", {}});
        } else if (action == "cancel_or_quit") {
            add(agent::KeyBinding{it.key(), "ui.cancel_or_quit", "", "global", "always", {}});
        } else if (action == "delete_word") {
            add(agent::KeyBinding{it.key(), "ui.input.delete_word", "", "input", "always", {}});
        } else if (action == "toggle_mouse") {
            add(agent::KeyBinding{it.key(), "ui.mouse.toggle", "", "global", "always", {}});
        }
    }
    add_legacy_defaults();
}

void KeyBinder::add_legacy_defaults() {
    for (int i = 1; i <= 9; ++i)
        add(agent::KeyBinding{"esc+" + std::to_string(i),
                              "core.window.set",
                              std::to_string(i),
                              "global",
                              "always",
                              {}});
    add(agent::KeyBinding{"esc+b", "ui.input.delete_word", "", "input", "always", {}});
    add(agent::KeyBinding{"esc+m", "ui.mouse.toggle", "", "global", "always", {}});
    add(agent::KeyBinding{"esc", "ui.escape", "drawer", "input", "drawer", {}});
    add(agent::KeyBinding{"esc", "ui.escape", "busy", "global", "busy", {}});
    add(agent::KeyBinding{"esc", "ui.escape", "idle", "global", "idle", {}});
}

agent::KeyBinding KeyBinder::find(const std::string& key, const InputState& state) const {
    for (const auto& binding : bindings_) {
        if (binding.key != key)
            continue;
        if (binding.scope != "global" && binding.scope != state.scope)
            continue;
        if (!when_matches(binding.when, state))
            continue;
        return binding;
    }
    return {};
}

agent::KeyBinding KeyBinder::dispatch(const KeyRead& key, const InputState& state) const {
    if (key.key >= 0xB0 && key.key <= 0xB9) {
        const std::string name = "alt+" + std::string(1, static_cast<char>('0' + (key.key - 0xB0)));
        auto binding = find(name, state);
        if (!binding.action.empty())
            return binding;
        // A meta digit is reserved for window switching: a missing binding
        // must not fall through to a typed character.
        return {};
    }
    if (key.key == 27) {
        // Drawer first: ESC is the close key while the drawer owns the screen,
        // even when the terminal already delivered the next byte. That was the
        // coded order in v1 and it is what keeps ESC+1 from switching windows
        // out from under an open completion list.
        if (state.drawer_open) {
            auto drawer = find("esc", state);
            if (!drawer.action.empty())
                return drawer;
        }
        if (key.followup && esc_prefix_key(*key.followup)) {
            const std::string name =
                "esc+" + lowercase(std::string(1, static_cast<char>(*key.followup)));
            return find(name, state);
        }
        return find("esc", state);
    }
    if (key.key >= 1 && key.key <= 26) {
        const char letter = static_cast<char>('a' + (key.key - 1));
        return find("ctrl+" + std::string(1, letter), state);
    }
    return {};
}

} // namespace tui
