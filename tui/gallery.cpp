#include "tui/gallery.h"

#include "tui/glyphs.h"

#include <algorithm>
#include <string>

namespace tui {

namespace {

std::string repeated(const char* glyph, int count) {
    std::string text;
    for (int i = 0; i < count; ++i)
        text += glyph;
    return text;
}

const char* mode_label(GlyphMode mode) {
    return mode == GlyphMode::Unicode ? "Unicode" : "ASCII";
}

void draw_text(Surface& surface, int x, int y, const std::string& text, Role role) {
    surface.put_text(x, y, text, Style{role});
}

void paint_shell(Surface& surface, const Glyphs& glyphs, GlyphMode mode, ThemeKind kind) {
    const int width = surface.width();
    draw_text(surface, 1, 0, "Amber render gallery", Role::Title);
    const std::string label = std::string(theme_kind_name(kind)) +
                              (mode == GlyphMode::Unicode ? " \u00b7 " : " - ") + mode_label(mode);
    draw_text(surface, std::max(1, width - static_cast<int>(label.size()) - 2), 0, label,
              Role::Muted);
    surface.draw_hline(1, 1, std::max(0, width - 2), glyphs.rule, Style{Role::Frame});
    surface.draw_box(Rect{1, 2, std::max(0, width - 2), std::max(0, surface.height() - 5)}, glyphs,
                     Style{Role::Frame});
}

void paint_roles(Surface& surface, int x, int y) {
    struct Sample {
        const char* label;
        Role role;
    };
    const Sample samples[] = {{"Text", Role::Text},       {"Muted", Role::Muted},
                              {"Accent", Role::Accent},   {"Attention", Role::Attention},
                              {"Success", Role::Success}, {"Error", Role::Error},
                              {"Warning", Role::Warning}};
    for (const Sample& sample : samples) {
        draw_text(surface, x, y, sample.label, sample.role);
        x += static_cast<int>(std::string(sample.label).size()) + 2;
    }
}

void paint_attributes(Surface& surface, int x, int y) {
    int attr_x = x;
    const Style bold{Role::Text, true};
    const Style italic{Role::Text, false, false, true};
    const Style under{Role::Text, false, false, false, true};
    const Style reverse{Role::Text, false, false, false, false, true};
    for (const auto& sample : {std::pair{"Bold", bold}, std::pair{"Italic", italic},
                               std::pair{"Underline", under}, std::pair{"Reverse", reverse}}) {
        surface.put_text(attr_x, y, sample.first, sample.second);
        attr_x += static_cast<int>(std::string(sample.first).size()) + 2;
    }
}

void paint_marks(Surface& surface, const Glyphs& glyphs, int x, int y) {
    surface.fill(Rect{x, y, std::max(0, surface.width() - x - 3), 1},
                 Cell{" ", Style{Role::Selected}});
    draw_text(surface, x, y, "Selected row (theme role)", Role::Selected);
    ++y;
    draw_text(surface, x, y,
              std::string("Scrollbar: ") + glyphs.filled + repeated(glyphs.shade, 8) + "  " +
                  glyphs.attention + " attention  " + glyphs.dot + " done  " + glyphs.cross_mark +
                  " error",
              Role::Text);
    ++y;
    draw_text(surface, x, y, "Components: measure / arrange / paint / handle / hit", Role::Text);
}

void paint_content(Surface& surface, const Glyphs& glyphs, GlyphMode mode, const Rect& frame) {
    const int x = frame.x + 2;
    int y = frame.y + 1;
    const auto row = [&](const std::string& text) {
        draw_text(surface, x, y, text, Role::Text);
        ++y;
    };

    draw_text(surface, x, y, "Frame: ", Role::Muted);
    draw_text(surface, x + 7, y,
              std::string(glyphs.top_left) + glyphs.hline + glyphs.tee_top + glyphs.hline +
                  glyphs.top_right + "  " + glyphs.vline + " " + glyphs.tee_left + " " +
                  glyphs.cross + " " + glyphs.tee_right + " " + glyphs.vline,
              Role::Frame);
    ++y;
    paint_roles(surface, x, y);
    paint_attributes(surface, x, ++y);
    ++y;
    row(std::string("Glyphs: ") + glyphs.tab_left + glyphs.tab_right + " " + glyphs.rule + " " +
        glyphs.filled + glyphs.shade + " " + glyphs.diamond + glyphs.dot + glyphs.cross_mark +
        glyphs.attention + " " + glyphs.arrow_up + glyphs.arrow_down + " " + glyphs.bullet +
        glyphs.ellipsis + " " + glyphs.spinner[0] + glyphs.spinner[1] + glyphs.spinner[2] +
        glyphs.spinner[3]);
    if (mode == GlyphMode::Unicode)
        row("Wide: 界界 (2 columns each)  e\u0301 (combining)");
    else
        row("Wide: ASCII mode has no double-width glyphs");
    paint_marks(surface, glyphs, x, y);
}

void paint_footer(Surface& surface, const Glyphs& glyphs, GlyphMode mode, ThemeKind kind) {
    draw_text(surface, 2, surface.height() - 2,
              std::string("theme: ") + theme_kind_name(kind) + "   a: " +
                  mode_label(mode == GlyphMode::Unicode ? GlyphMode::Ascii : GlyphMode::Unicode) +
                  "   t: next theme   Esc: quit",
              Role::Hint);
    surface.draw_hline(1, surface.height() - 3, std::max(0, surface.width() - 2), glyphs.hline,
                       Style{Role::Frame});
}

} // namespace

Surface render_gallery(Size size, GlyphMode mode, ThemeKind kind) {
    Surface surface(size.w, size.h);
    paint_gallery(surface, mode, kind);
    return surface;
}

void paint_gallery(Surface& surface, GlyphMode mode, ThemeKind kind) {
    const Glyphs& glyphs = mode == GlyphMode::Unicode ? Glyphs::unicode() : Glyphs::ascii();
    surface.clear(Cell{" ", Style{Role::Surface}});
    paint_shell(surface, glyphs, mode, kind);
    const Rect frame{1, 2, std::max(0, surface.width() - 2), std::max(0, surface.height() - 5)};
    if (surface.width() < 24 || surface.height() < 12) {
        draw_text(surface, 2, 3, "too small for the gallery", Role::Error);
        return;
    }
    paint_content(surface, glyphs, mode, frame);
    paint_footer(surface, glyphs, mode, kind);
}

} // namespace tui
