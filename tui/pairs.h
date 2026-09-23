#ifndef AMBER_TUI_PAIRS_H
#define AMBER_TUI_PAIRS_H

// Colour-pair identifiers used across the whole TUI. Pure: these are just ids,
// so a module that only needs to *name* a pair (rich lines, markdown runs) can
// include this without pulling in <ncurses.h>. The ncurses side lives in
// widgets.h (init_pairs). Call init_pairs() once, after start_color(), to
// register them.
namespace tui {

enum Pair {
    P_USER = 1,   // user text
    P_ASSISTANT,  // assistant text
    P_STATUS,     // tool / status
    P_DEBUG,      // debug trace (magenta)
    P_REASONING,  // model thinking / reasoning (dim)
    P_BANNER,     // banner / status bar
    P_FIELD,      // editable form field (black background)
    P_FIELD_ACT,  // active/focused form field
    P_DIALOG,     // dialog body
    P_BUTTON,     // button (unfocused)
    P_BUTTON_ACT, // button (focused)
    P_SHADOW,     // drop shadow
    // Status-bar gauge / state segments (colored foreground on the blue bar).
    P_GAUGE_OK,   // context gauge, low pressure   (green on blue)
    P_GAUGE_WARN, // context gauge, mid pressure    (yellow on blue)
    P_GAUGE_CRIT, // context gauge, high pressure   (red on blue)
    P_BAR_DIM,    // dim gauge track / faint labels (cyan on blue)
    P_GIT_PLUS,   // git added lines (green)
    P_GIT_MINUS,  // git deleted lines (red)
    // Markdown rendering pairs (chat canvas).
    P_MD_HEAD,      // headings
    P_MD_QUOTE,     // block quote
    P_MD_CODE,      // inline code / fenced block
    P_MD_CODEKEY,   // code highlight: keyword
    P_MD_CODESTR,   // code highlight: string
    P_MD_CODENUM,   // code highlight: number
    P_MD_CODECMT,   // code highlight: comment
    P_MD_LINK,      // link text
    P_MD_TABLE,     // table rows
    P_MD_HR,        // horizontal rule
    P_INPUT_SHADOW, // faded completion hint (gray on default bg)
    // Grayscale art pairs (24 levels, 0=black .. 23=near-white).
    P_GRAY = 100, // grayscale art base (0 + 24 levels)
};

} // namespace tui

#endif // AMBER_TUI_PAIRS_H
