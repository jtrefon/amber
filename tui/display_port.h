#ifndef AMBER_TUI_DISPLAY_PORT_H
#define AMBER_TUI_DISPLAY_PORT_H

#include <string>

namespace tui {

// Port: display output. The domain core uses this to render without
// depending on ncurses or RenderEngine.
class DisplayPort {
public:
    virtual ~DisplayPort() = default;
    virtual void draw() = 0;
    virtual void draw_input(const std::string& line) = 0;
    virtual void flush() = 0;
    virtual void set_scroll_mode(bool on) = 0;
    virtual bool scroll_mode() const = 0;
    virtual int max_scroll() const = 0;
    virtual bool dirty() const = 0;
};

} // namespace tui

#endif // AMBER_TUI_DISPLAY_PORT_H
