// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

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

} // namespace text
} // namespace tui

#endif // AMBER_TUI_TEXTUTIL_H
