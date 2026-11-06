#ifndef AMBER_TUI_GLYPHS_H
#define AMBER_TUI_GLYPHS_H

// The shell's glyph vocabulary, in two explicit sets. Locale detection is not
// used here: a test and a gallery must render the same cells everywhere, and a
// caller that wants the terminal's fallback asks for ASCII by name. Every
// screen draws from this struct rather than spelling a box character inline.

namespace tui {

struct Glyphs {
    const char* hline;
    const char* vline;
    const char* top_left;
    const char* top_right;
    const char* bottom_left;
    const char* bottom_right;
    const char* tee_left;
    const char* tee_right;
    const char* tee_top;
    const char* tee_bottom;
    const char* cross;
    const char* rule;      // the strong horizontal rule under the tab rail
    const char* tab_left;  // the cap that opens the active tab
    const char* tab_right; // the cap that closes it
    const char* filled;    // scrollbar thumb
    const char* shade;     // scrollbar track
    const char* diamond;   // the amber mark
    const char* dot;       // completed / unread
    const char* cross_mark;
    const char* attention;
    const char* arrow_up;
    const char* arrow_down;
    const char* bullet;
    const char* ellipsis;
    const char* spinner[4];

    static const Glyphs& unicode();
    static const Glyphs& ascii();
};

} // namespace tui

#endif // AMBER_TUI_GLYPHS_H
