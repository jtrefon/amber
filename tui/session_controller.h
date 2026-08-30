#ifndef AMBER_TUI_SESSION_CONTROLLER_H
#define AMBER_TUI_SESSION_CONTROLLER_H

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace agent {
struct Session;
struct Message;
class SessionStore;
} // namespace agent

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
    void lazy_load_active();
    void save_workspace_now();
    void redraw_after_modal();

private:
    struct RestoredCall {
        std::string name;
        nlohmann::json args;
    };
    void restore_message_lines(const agent::Message& m, std::vector<RestoredCall>& pending);

    Tui& tui_;
};

} // namespace tui

#endif
