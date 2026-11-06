#include "tui/glyphs.h"

namespace tui {

const Glyphs& Glyphs::unicode() {
    static const Glyphs set{
        "\u2500", "\u2502", "\u250c", "\u2510", "\u2514",
        "\u2518", "\u251c", "\u2524", "\u252c", "\u2534",
        "\u253c", "\u2550", "\u2590", "\u258c", "\u2588",
        "\u2591", "\u25c6", "\u25cf", "\u2717", "!",
        "\u25b2", "\u25bc", "\u2022", "\u2026", {"\u25d0", "\u25d3", "\u25d1", "\u25d2"}};
    return set;
}

const Glyphs& Glyphs::ascii() {
    static const Glyphs set{"-", "|", "+", "+",   "+",
                            "+", "+", "+", "+",   "+",
                            "+", "=", "|", "|",   "#",
                            "-", "*", "o", "x",   "!",
                            "^", "v", "*", "...", {"-", "\\", "|", "/"}};
    return set;
}

} // namespace tui
