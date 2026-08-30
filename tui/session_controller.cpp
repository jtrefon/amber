#include "session_controller.h"
#include "tui.h"

namespace tui {

SessionController::SessionController(Tui& tui) : tui_(tui) {}

agent::Session SessionController::snapshot(Window& w) const { return tui_.snapshot(w); }
void SessionController::autosave() { tui_.autosave(); }
void SessionController::autosave(Window& w) { tui_.autosave(w); }
void SessionController::save_window_sessions() { tui_.save_window_sessions(); }
void SessionController::save_session() { tui_.save_session(); }
void SessionController::load_session(const std::string& id) { tui_.load_session(id); }
void SessionController::session_browser() { tui_.session_browser(); }
void SessionController::lazy_load_active() { tui_.lazy_load_active(); }
void SessionController::save_workspace_now() { tui_.save_workspace_now(); }
void SessionController::redraw_after_modal() { tui_.redraw_after_modal(); }

void SessionController::restore_message_lines(const agent::Message& m, std::vector<RestoredCall>& pending) {
    // Delegates to Tui's implementation; kept here for future extraction.
    (void)m;
    (void)pending;
}

} // namespace tui
