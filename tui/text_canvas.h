#ifndef AMBER_TUI_TEXT_CANVAS_H
#define AMBER_TUI_TEXT_CANVAS_H

// The chat scrollback on the cell grid. It stores rich lines, wraps them once
// per width, and paints only the viewport into a Surface. Nothing here owns a
// curses window: the renderer blits the surface with the rest of the frame.
//
// Wrapping is cached. `set_lines` compares the new view with the stored one and
// only marks the wrap dirty when it differs, so a scroll key (or a steady-state
// repaint that rebuilds an identical view) never re-wraps 10k lines. A width
// change is the other invalidation; the next paint rewraps exactly once.

#include "tui/rich.h"
#include "tui/surface.h"

#include <cstddef>
#include <utility>
#include <vector>

namespace tui {

class TextCanvas {
public:
    TextCanvas() = default;

    // The viewport the renderer wants filled. Height is the visible rows, not a
    // screen offset; the caller blits the surface at its own top row.
    void set_viewport(int rows, int cols);

    // Replace the source lines. A taken-by-value argument lets the renderer
    // hand over the view it just built without a second copy.
    void set_lines(std::vector<rich::Line> lines);

    // First wrapped row shown, clamped to the current wrap.
    int top();
    void set_top(int top);
    int max_top();
    int wrapped_count();
    int rows() const noexcept { return rows_; }
    int cols() const noexcept { return cols_; }

    // Number of wraps performed. A test can prove that scrolling, or a repaint
    // that supplies the same lines, did not wrap again.
    std::size_t wrap_count() const noexcept { return wrap_count_; }

    // Paint the viewport into the canvas surface and return it for the frame's
    // blit. The surface is reused between paints.
    const Surface& surface();

private:
    void ensure_wrapped();

    int rows_ = 0;
    int cols_ = 0;
    int top_ = 0;
    bool wrap_dirty_ = true;
    std::size_t wrap_count_ = 0;
    std::vector<rich::Line> lines_;
    std::vector<rich::Line> wrapped_;
    Surface surface_{0, 0};
};

} // namespace tui

#endif // AMBER_TUI_TEXT_CANVAS_H
