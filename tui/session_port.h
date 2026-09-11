#ifndef AMBER_TUI_SESSION_PORT_H
#define AMBER_TUI_SESSION_PORT_H

#include <optional>
#include <string>

namespace tui {

// Port: session/agent lifecycle. The domain core uses this to check
// busy state, drain events, poll balance, and handle signals without
// depending on EventRouter or the agent core directly.
class SessionPort {
public:
    virtual ~SessionPort() = default;
    virtual bool is_busy() const = 0;
    virtual void drain_events() = 0;
    virtual void check_timeouts() = 0;
    virtual void poll_balance() = 0;
    virtual bool consume_signal() = 0;  // returns true if a deferred signal fired
    virtual void handle_shutdown() = 0;
};

} // namespace tui

#endif // AMBER_TUI_SESSION_PORT_H
