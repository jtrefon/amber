#ifndef AMBER_TUI_COMPOSITOR_H
#define AMBER_TUI_COMPOSITOR_H

// Layer compositing for cell grids. A layer is a surface plus the destination
// rect it occupies; lower z paints first. Clipping never copies half a wide
// glyph: a lead whose continuation falls outside the visible area is skipped,
// and a continuation whose lead was clipped is skipped too.

#include "tui/geometry.h"
#include "tui/surface.h"

#include <cstddef>
#include <vector>

namespace tui {

class Compositor {
public:
    void add(const Surface& surface, const Rect& destination, int z = 0);
    void clear() noexcept { layers_.clear(); }
    void composite(Surface& target) const;

private:
    struct Layer {
        const Surface* surface = nullptr;
        Rect destination;
        int z = 0;
        std::size_t order = 0;
    };
    std::vector<Layer> layers_;
};

} // namespace tui

#endif // AMBER_TUI_COMPOSITOR_H
