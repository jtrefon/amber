#ifndef AGENT_KEYBINDING_H
#define AGENT_KEYBINDING_H

#include <string>

namespace agent {

// One key -> action mapping. `scope` is the input surface the binding applies
// to: `global` matches every surface, `input` the prompt, and `menu`, `dialog`
// and `list` the corresponding local loops. `when` is the state predicate the
// binder evaluates before choosing the binding (`always`, `drawer`, `busy`,
// `idle`). The action is the same ID a menu or slash command dispatches, so a
// keybinding never opens a second action surface.
struct KeyBinding {
    std::string key;
    std::string action;
    std::string arg;
    std::string scope = "global";
    std::string when = "always";
    std::string owner;
};

} // namespace agent

#endif // AGENT_KEYBINDING_H
