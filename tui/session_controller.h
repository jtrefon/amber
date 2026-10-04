#ifndef AMBER_TUI_SESSION_CONTROLLER_H
#define AMBER_TUI_SESSION_CONTROLLER_H

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <agent/session.h>

namespace tui {
class Tui;
struct Window;

class SessionBrowserCore;

class SessionController {
public:
    explicit SessionController(Tui& tui);
    agent::Session snapshot(Window& w) const;
    void autosave();
    void autosave(Window& w);
    void save_window_sessions();
    void save_session();
    void load_session(const std::string& id);
    // Split the active window's session: a new window gets an identical
    // context copy (the shared wire prefix is preserved byte-for-byte so
    // the fork's first request hits the server prefix cache), its own
    // session id, and a `forked_from` lineage marker. Both legs diverge
    // independently afterward. Rejected while the source agent runs —
    // Context is single-owner and cannot be snapshotted mid-mutation.
    void fork_session();
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
    // Project a loaded session into a window: context, meta, per-turn UI stats,
    // scrollback, spinner rows and the rebuilt call list. Tui::lazy_load_active
    // used to inline all of this *and* duplicate restore_stats(), which is how the
    // two paths could drift apart.
    void restore_into(Window& w, const agent::Session& s);

private:
    // Restore one saved message: user text, assistant text, or a tool result.
    void restore_user_line(const agent::Message& m);
    void collect_tool_calls(const agent::Message& m, std::vector<RestoredCall>& pending);
    void restore_tool_line(const agent::Message& m, std::vector<RestoredCall>& pending);

    // Background-compress a freshly loaded session when its context is large.
    void maybe_background_compress(Window& w, const agent::Session& s);
    // Restore the window's UI state (context use, latency/token stats) from
    // the saved session meta.
    void restore_stats(Window& w, const agent::Session& s);

    // The browser's key step (takes the raw key so the header needs no ncurses):
    // returns true when the dialog should close.
    bool session_browser_key(int ch, SessionBrowserCore& core);
    // Confirm and delete the selected session; true when the list became empty.
    bool confirm_delete_session(SessionBrowserCore& core);

    Tui& tui_;
    agent::SessionStore store_;
    std::string settings_path_;
};

} // namespace tui

#endif