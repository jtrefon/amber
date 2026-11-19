#ifndef AMBER_TUI_PROFILE_COMMANDS_H
#define AMBER_TUI_PROFILE_COMMANDS_H

#include <string>

namespace tui {

class SlashDispatcher;
class Tui;

// /profile's handlers, split out of SlashDispatcher so the dispatcher stays
// under the 200-line type cap. One domain, one owner: the store and masking
// live in lib/profiles, this class is only the UI surface.
class ProfileSurface {
public:
    explicit ProfileSurface(Tui& tui) : ui_(tui) {}

    void register_actions(SlashDispatcher& dispatcher);
    // Live leaves: every profile name hangs under switch/show/remove with a
    // host-derived action. Regenerated after add/remove/switch so the drawer
    // always matches the store.
    void refresh_feed();
    void show(const std::string& name);
    void list();
    void add(const std::string& name);
    void save();
    void switch_to(const std::string& name);
    // Apply the project's .amber/profile override, if it names another profile.
    void switch_project(const std::string& root);
    void remove(const std::string& name);
    // Apply a switch queued while the active window was busy. Runs on the UI
    // thread at the safe boundary: the run slot is idle, so no worker holds the
    // client being replaced.
    void apply_pending();

private:
    void apply(const std::string& name);
    Tui& ui_;
    SlashDispatcher* dispatcher_ = nullptr;
    std::string pending_;
};

} // namespace tui

#endif // AMBER_TUI_PROFILE_COMMANDS_H
