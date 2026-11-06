// The render foundation is pure: geometry, an ncurses-free cell grid, a
// compositor, theme roles, a glyph set and a component base. These tests pin
// the invariants the later screen migrations rely on — wide glyphs and their
// continuation cells, clipping, layer order and focus/hit dispatch — without a
// TTY. The gallery test renders every primitive at both target sizes and in
// both glyph modes.

#include "tui/cell.h"
#include "tui/component.h"
#include "tui/compositor.h"
#include "tui/gallery.h"
#include "tui/geometry.h"
#include "tui/glyphs.h"
#include "tui/surface.h"
#include "tui/theme.h"
#include "test_util.h"

#include <algorithm>
#include <string>

namespace {

using tui::Cell;
using tui::Compositor;
using tui::Event;
using tui::FocusManager;
using tui::Glyphs;
using tui::HitMap;
using tui::Rect;
using tui::Role;
using tui::Size;
using tui::Style;
using tui::Surface;
using tui::Theme;

// A component small enough to prove arrange/paint without a layout engine: a
// filled rect with its index in the corner.
class Tile : public tui::Component {
public:
    Tile(int number, bool focusable) : number_(number) { focusable_ = focusable; }
    tui::Size measure(const tui::Size& available) const override {
        return {std::min(available.w, 8), std::min(available.h, 3)};
    }
    void paint(Surface& surface) const override {
        surface.fill(bounds_, Cell{" ", Style{Role::Accent}});
        surface.put_text(bounds_.x + 1, bounds_.y + 1, std::to_string(number_), Style{Role::Text});
    }
    bool focusable() const override { return focusable_; }

private:
    int number_ = 0;
    bool focusable_ = true;
};

} // namespace

// ---------------------------------------------------------------------------
// Geometry and glyph widths
// ---------------------------------------------------------------------------

TEST(rect_intersect_clips_to_the_overlap) {
    const Rect a{2, 3, 10, 4};
    const Rect b{5, 1, 3, 10};
    const Rect hit = a.intersect(b);
    ASSERT(hit.x == 5 && hit.y == 3 && hit.w == 3 && hit.h == 4);

    ASSERT(a.contains(2, 3));
    ASSERT(a.contains(11, 6));
    ASSERT(!a.contains(12, 6));
    ASSERT(!a.contains(2, 7));
    ASSERT((Rect{0, 0, 0, 5}.empty()));
    ASSERT((Rect{0, 0, 5, 0}.empty()));
}

TEST(glyph_width_counts_wide_and_combining_cells) {
    ASSERT_EQ(tui::glyph_width('a'), 1);
    ASSERT_EQ(tui::glyph_width(0x754C), 2);  // 界
    ASSERT_EQ(tui::glyph_width(0x1F600), 2); // emoji
    ASSERT_EQ(tui::glyph_width(0x0301), 0);  // combining acute
    ASSERT_EQ(tui::text_columns("a界b"), 4);
}

// ---------------------------------------------------------------------------
// Surface: wide glyphs, continuation cells and repair
// ---------------------------------------------------------------------------

TEST(surface_put_text_marks_the_continuation_cell) {
    Surface surface(6, 1);
    const int used = surface.put_text(0, 0, "a界b", Style{Role::Text});
    ASSERT_EQ(used, 4);
    ASSERT_EQ(surface.at(0, 0).glyph, std::string("a"));
    ASSERT_EQ(surface.at(1, 0).glyph, std::string("界"));
    ASSERT(!surface.at(1, 0).continuation);
    ASSERT(surface.at(2, 0).continuation);
    ASSERT_EQ(surface.at(2, 0).glyph, std::string());
    ASSERT_EQ(surface.at(3, 0).glyph, std::string("b"));
}

TEST(surface_overwriting_half_a_wide_glyph_repairs_both_cells) {
    Surface surface(4, 1);
    surface.put_text(0, 0, "界", Style{});
    ASSERT(surface.at(1, 0).continuation);

    // Writing the second half must clear the orphaned first half.
    surface.put(1, 0, Cell{"x", Style{Role::Text}});
    ASSERT_EQ(surface.at(0, 0).glyph, std::string(" "));
    ASSERT(!surface.at(0, 0).continuation);
    ASSERT_EQ(surface.at(1, 0).glyph, std::string("x"));
}

TEST(surface_put_text_clips_without_splitting_a_wide_glyph) {
    Surface surface(3, 1);
    const int used = surface.put_text(0, 0, "界界", Style{});
    ASSERT_EQ(used, 2); // the second 界 needs two columns; one is left
    ASSERT_EQ(surface.at(0, 0).glyph, std::string("界"));
    ASSERT(surface.at(1, 0).continuation);
    ASSERT_EQ(surface.at(2, 0).glyph, std::string(" "));

    // A negative start clips the left edge; a whole wide glyph is not copied
    // from its second half.
    Surface left(4, 1);
    const int used_left = left.put_text(-1, 0, "a界b", Style{});
    ASSERT_EQ(used_left, 3);
    ASSERT_EQ(left.at(0, 0).glyph, std::string("界"));
    ASSERT(left.at(1, 0).continuation);
    ASSERT_EQ(left.at(2, 0).glyph, std::string("b"));
}

TEST(surface_diff_erases_a_stale_wide_tail) {
    Surface surface(4, 1);
    surface.put_text(0, 0, "界", Style{});
    surface.commit();

    surface.clear();
    surface.put_text(0, 0, "A", Style{});
    const auto damage = surface.diff();

    bool lead = false;
    bool tail = false;
    for (const auto& d : damage) {
        if (d.x == 0 && d.y == 0 && d.cell.glyph == "A")
            lead = true;
        if (d.x == 1 && d.y == 0 && d.cell.glyph == " " && !d.cell.continuation)
            tail = true;
    }
    ASSERT(lead);
    ASSERT(tail);
}

TEST(surface_text_helpers_and_box_drawing) {
    Surface surface(8, 3);
    surface.fill({0, 0, 8, 3}, Cell{" ", Style{Role::Surface}});
    surface.draw_box({0, 0, 8, 3}, Glyphs::unicode(), Style{Role::Frame});
    ASSERT_EQ(surface.at(0, 0).glyph, std::string("┌"));
    ASSERT_EQ(surface.at(7, 0).glyph, std::string("┐"));
    ASSERT_EQ(surface.at(0, 2).glyph, std::string("└"));
    ASSERT_EQ(surface.at(7, 2).glyph, std::string("┘"));
    ASSERT_EQ(surface.at(3, 0).glyph, std::string("─"));

    surface.put_text(1, 1, "hi", Style{Role::Text});
    ASSERT_EQ(surface.row_text(1), std::string("│hi    │"));
}

// ---------------------------------------------------------------------------
// Compositor
// ---------------------------------------------------------------------------

TEST(compositor_orders_layers_and_clips_to_the_target) {
    Surface base(4, 2);
    base.fill({0, 0, 4, 2}, Cell{".", Style{Role::Surface}});
    Surface low(2, 1);
    low.fill({0, 0, 2, 1}, Cell{"L", Style{Role::Accent}});
    Surface high(2, 1);
    high.fill({0, 0, 2, 1}, Cell{"H", Style{Role::Attention}});

    Compositor compositor;
    compositor.add(low, Rect{0, 0, 2, 1}, 0);
    compositor.add(high, Rect{1, 0, 2, 1}, 1); // overlaps low, clipped at x=4
    compositor.composite(base);

    ASSERT_EQ(base.at(0, 0).glyph, std::string("L"));
    ASSERT_EQ(base.at(1, 0).glyph, std::string("H"));
    ASSERT_EQ(base.at(2, 0).glyph, std::string("H"));
    ASSERT_EQ(base.at(3, 0).glyph, std::string("."));
    ASSERT_EQ(base.at(0, 1).glyph, std::string("."));
}

TEST(compositor_never_copies_half_a_wide_glyph) {
    Surface base(4, 1);
    base.fill({0, 0, 4, 1}, Cell{" ", Style{Role::Surface}});
    Surface layer(2, 1);
    layer.put_text(0, 0, "界", Style{Role::Accent});

    // Clipped at the left: only the continuation would land in column 0.
    Compositor left;
    left.add(layer, Rect{-1, 0, 2, 1}, 0);
    left.composite(base);
    ASSERT_EQ(base.at(0, 0).glyph, std::string(" "));

    // Clipped at the right: the lead would land in the last column alone.
    Surface base2(2, 1);
    base2.fill({0, 0, 2, 1}, Cell{" ", Style{Role::Surface}});
    Compositor right;
    right.add(layer, Rect{1, 0, 2, 1}, 0);
    right.composite(base2);
    ASSERT_EQ(base2.at(1, 0).glyph, std::string(" "));
}

// ---------------------------------------------------------------------------
// Theme and glyphs
// ---------------------------------------------------------------------------

TEST(theme_roles_are_semantic_and_modes_differ) {
    const Theme amber = Theme::amber(tui::GlyphMode::Unicode);
    ASSERT(amber.style(Role::Frame).fg != amber.style(Role::Surface).fg);
    ASSERT(amber.style(Role::Attention).bold);
    ASSERT(amber.style(Role::Selected).reverse ||
           amber.style(Role::Selected).bg != amber.style(Role::Surface).bg);

    ASSERT_EQ(Glyphs::unicode().top_left, std::string("┌"));
    ASSERT_EQ(Glyphs::ascii().top_left, std::string("+"));
    ASSERT_EQ(Glyphs::ascii().hline, std::string("-"));
    ASSERT_EQ(Glyphs::ascii().vline, std::string("|"));
    for (const char* glyph : Glyphs::ascii().spinner)
        ASSERT(glyph[1] == '\0');
}

// ---------------------------------------------------------------------------
// Components, focus and hit testing
// ---------------------------------------------------------------------------

TEST(component_arranges_measures_and_paints_inside_its_bounds) {
    Tile tile(7, true);
    ASSERT(tile.measure(Size{80, 24}).w == 8);
    ASSERT(tile.measure(Size{4, 1}).w == 4);
    tile.arrange(Rect{2, 1, 8, 3});

    Surface surface(12, 5);
    surface.fill({0, 0, 12, 5}, Cell{" ", Style{Role::Surface}});
    tile.paint(surface);
    ASSERT_EQ(surface.at(3, 2).glyph, std::string("7"));
    ASSERT_EQ(surface.at(11, 4).glyph, std::string(" ")); // untouched outside
}

TEST(focus_manager_cycles_focusable_components_and_dispatches) {
    Tile first(1, true);
    Tile skipped(2, false);
    Tile last(3, true);
    FocusManager focus;
    focus.add(&first);
    focus.add(&skipped);
    focus.add(&last);

    ASSERT(focus.focused() == nullptr);
    ASSERT(focus.next() == &first);
    ASSERT(focus.next() == &last);  // the unfocusable tile is skipped
    ASSERT(focus.next() == &first); // wraps
    ASSERT(focus.prev() == &last);

    ASSERT(focus.focus(&skipped) == false);
    ASSERT(focus.focused() == &last);
}

TEST(hit_map_returns_the_topmost_component) {
    Tile bottom(1, true);
    Tile top(2, true);
    HitMap map;
    map.add(Rect{0, 0, 4, 2}, &bottom);
    map.add(Rect{2, 1, 3, 2}, &top);

    ASSERT(map.at(1, 0) == &bottom);
    ASSERT(map.at(2, 1) == &top);
    ASSERT(map.at(3, 0) == &bottom);
    ASSERT(map.at(6, 1) == nullptr);
}

// ---------------------------------------------------------------------------
// Gallery
// ---------------------------------------------------------------------------

TEST(gallery_renders_every_primitive_at_both_target_sizes) {
    for (const tui::GlyphMode mode : {tui::GlyphMode::Unicode, tui::GlyphMode::Ascii}) {
        for (const Size size : {Size{80, 24}, Size{120, 40}}) {
            const Surface surface = tui::render_gallery(size, mode);
            ASSERT(surface.width() == size.w && surface.height() == size.h);

            bool title = false;
            bool wide = false;
            bool bold = false;
            bool underline = false;
            bool reverse = false;
            for (int y = 0; y < surface.height(); ++y) {
                const std::string row = surface.row_text(y);
                title = title || row.find("Amber render gallery") != std::string::npos;
                for (int x = 0; x < surface.width(); ++x) {
                    const tui::Cell& cell = surface.at(x, y);
                    wide = wide || cell.continuation;
                    bold = bold || cell.style.bold;
                    underline = underline || cell.style.underline;
                    reverse = reverse || cell.style.reverse;
                }
            }
            ASSERT(title);
            ASSERT(bold && underline && reverse);
            if (mode == tui::GlyphMode::Unicode)
                ASSERT(wide); // the wide-glyph sample is present
            else
                ASSERT(!wide); // ASCII has no double-width glyphs
        }
    }
}

TEST(gallery_ascii_mode_stays_ascii) {
    const Surface surface = tui::render_gallery(Size{80, 24}, tui::GlyphMode::Ascii);
    for (int y = 0; y < surface.height(); ++y) {
        const std::string row = surface.row_text(y);
        for (const char c : row)
            ASSERT(static_cast<unsigned char>(c) < 0x80);
    }
}
