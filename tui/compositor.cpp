#include "tui/compositor.h"

#include <algorithm>

namespace tui {

void Compositor::add(const Surface& surface, const Rect& destination, int z) {
    layers_.push_back(Layer{&surface, destination, z, layers_.size()});
}

void Compositor::composite(Surface& target) const {
    std::vector<const Layer*> ordered;
    ordered.reserve(layers_.size());
    for (const Layer& layer : layers_)
        ordered.push_back(&layer);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const Layer* a, const Layer* b) { return a->z < b->z; });

    const Rect target_bounds{0, 0, target.width(), target.height()};
    for (const Layer* layer : ordered) {
        if (!layer->surface)
            continue;
        const Rect area = layer->destination.intersect(target_bounds);
        for (int y = area.y; y < area.bottom(); ++y) {
            for (int x = area.x; x < area.right(); ++x) {
                const int sx = x - layer->destination.x;
                const int sy = y - layer->destination.y;
                const Cell& cell = layer->surface->at(sx, sy);
                if (cell.continuation)
                    continue; // the lead column is outside the copied area
                const bool wide = layer->surface->at(sx + 1, sy).continuation;
                if (wide && x + 1 >= area.right())
                    continue; // the second column would be clipped off
                target.put(x, y, cell);
            }
        }
    }
}

} // namespace tui
