#ifndef AMBER_TUI_SESSION_CONTROLLER_H
#define AMBER_TUI_SESSION_CONTROLLER_H

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <agent/session.h>

namespace tui {
class Tui;
struct Window;

class SessionController {
public:
    explicit SessionController(Tui& tui);
    agent::Session snapshot(Window& w) const;
    void autosave();
    void autosave(Window& w);
    void save_window_sessions();
    void save_session();
    void load_session(const std::string& id);
    void session_browser();
    void save_workspace_now();
    void redraw_after_modal();
    agent::WorkspaceState load_workspace();
    agent::SessionStore& store() noexcept { return store_; }
    std::string settings_path() const { return settings_path_; }
    struct RestoredCall {
        std::string name;
        nlohmann::json args;
    };
    void restore_message_lines(const agent::Message& m, std::vector<RestoredCall>& pending);

private:
    Tui& tui_;
    agent::SessionStore store_;
    std::string settings_path_;
public:
    void init_path(const std::string& p) { settings_path_ = p; }
};

} // namespace tui

#endif