
#ifndef AMBER_TUI_TEXTUTIL_H
#define AMBER_TUI_TEXTUTIL_H

#include <cstddef>
#include <string>
#include <vector>

// UTF-8 aware text helpers for the terminal UI. Kept separate from the Tui
// class so the wrapping/width/decoding logic can be reasoned about and tested
// in isolation (single responsibility: text measurement and layout).
namespace tui {
namespace text {

// Byte length of the UTF-8 sequence starting at index i. A truncated or invalid
// sequence is treated as a single byte so callers always make progress.
std::size_t utf8_len(const std::string& s, std::size_t i);

// Number of display columns (whole UTF-8 characters, each counted as one).
int display_cols(const std::string& s);

// Word-wrap `text` to `w` display columns. Expands tabs, drops CR and other
// control bytes, strips ANSI CSI escape sequences, and never slices through a
// multibyte character. Returns one string per output line.
std::vector<std::string> wrap(const std::string& text, int w);

// Decode a UTF-8 byte string into Unicode code points for ncursesw's wide-char
// API (mvaddnwstr), which places each glyph in one cell correctly.
std::wstring to_wide(const std::string& s);

// UI glyph selection. Many SSH/PuTTY sessions run in a non-UTF-8 locale (or
// with a Latin-1 translation table), where raw UTF-8 bytes render as garbage
// (e.g. "M-b\M-(" instead of ✨). These helpers return an ASCII fallback when
// the active locale is not UTF-8, so decoration works on every terminal.
namespace glyph {

// True iff the process locale's character set is UTF-8.
bool utf8();

// Sparkle bullet used to prefix tool-call / tool-result lines.
const char* tool();

// Right arrow used in tool-result summaries ("→ ✓").
const char* arrow();

// Middle dot used as a separator in the welcome banner.
const char* middot();

// Em dash used as a placeholder / separator in the status bar.
const char* emdash();

// Up / down arrows used in the stats gauge.
const char* up();
const char* down();

// Left / right half-block used to build the activity gauge.
const char* block_l();
const char* block_r();

// Horizontal ellipsis used for truncation in lists.
const char* ellipsis();

// Checkmark used for tool-result success indicators.
const char* check();

// Cross used for tool-result failure indicators.
const char* cross();

// Animated spinner frames. Square corners are used for core tools, round
// for bash, so the running tool's class is visible at a glance. Frames
// cycle mod 4; the ASCII fallback cycles |/-\ on non-UTF-8 terminals.
const char* spinner_square(int frame);
const char* spinner_round(int frame);

} // namespace glyph

// Build the git-aware decorated prompt string.
// Project is the folder basename, branch is git branch, ins/del from diff.
// Returns e.g. "┌ project branch +3/-1 ❯ " or ASCII fallback.
std::string git_prompt(const std::string& project, const std::string& branch,
                       int ins, int del);

// Convert a display-column offset to a byte offset in a UTF-8 string.
// Returns the byte position (npos if col exceeds the string's display width).
size_t col_to_byte(const std::string& s, int col);

} // namespace text
} // namespace tui

#endif // AMBER_TUI_TEXTUTIL_H
