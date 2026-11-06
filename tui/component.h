#ifndef AMBER_TUI_COMPONENT_H
#define AMBER_TUI_COMPONENT_H

// A component is the unit later screens compose: it measures itself against the
// available space, is arranged into a rect, paints into a cell grid, and handles
// an input event. The base owns only the bounds; focus and hit testing are two
// small managers so a screen does not grow its own bookkeeping.

#include "tui/cell.h"
#include "tui/geometry.h"
#include "tui/surface.h"

#include <cstddef>
#include <vector>

namespace tui {

struct Event {
    enum class Kind { Key, Mouse };
    Kind kind = Kind::Key;
    int key = 0;
    Point at;
};

class Component {
public:
    virtual ~Component() = default;

    // Preferred size given what is available. The default takes the space.
    virtual Size measure(const Size& available) const { return available; }
    virtual void arrange(const Rect& bounds) { bounds_ = bounds; }
    Rect bounds() const noexcept { return bounds_; }

    virtual void paint(Surface& surface) const = 0;
    virtual bool handle(const Event&) { return false; }
    virtual bool focusable() const { return false; }

protected:
    Rect bounds_;
};

class FocusManager {
public:
    void add(Component* component);
    void clear() noexcept;
    Component* focused() const noexcept;
    bool focus(Component* component);
    Component* next();
    Component* prev();
    bool dispatch(const Event& event);

private:
    Component* step(int direction);
    std::vector<Component*> components_;
    int focused_ = -1;
};

// Topmost-wins hit testing. A screen adds the rects in paint order and asks
// which component owns a cell; the map does no geometry beyond the rects it was
// given.
class HitMap {
public:
    void clear() noexcept { entries_.clear(); }
    void add(const Rect& area, Component* component);
    Component* at(int x, int y) const;

private:
    struct Entry {
        Rect area;
        Component* component = nullptr;
    };
    std::vector<Entry> entries_;
};

} // namespace tui

#endif // AMBER_TUI_COMPONENT_H
