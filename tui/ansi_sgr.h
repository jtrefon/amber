#ifndef AMBER_TUI_ANSI_SGR_H
#define AMBER_TUI_ANSI_SGR_H

#include <cstddef>
#include <string>
#include <vector>

#include "tui/markdown.h" // md::RunStyle, md::Style

// L1 (pure): parse ANSI SGR sequences and map their codes onto Run styles.
namespace tui::ansi_sgr {

// Parse the SGR sequence at `s[i]`, where `s[i]` is ESC and `s[i+1]` is '['.
// Returns the numeric parameters (empty parameters count as 0) and sets `next`
// past the sequence. An unterminated or malformed sequence yields what was
// read so far, so the caller always advances.
std::vector<int> parse(const std::string& s, std::size_t i, std::size_t& next);

// Apply one SGR code to `cur`, resetting to `base` on code 0. Colour codes map
// through the document's Style pairs.
void apply(int code, md::RunStyle& cur, const md::RunStyle& base, const md::Style& st);

} // namespace tui::ansi_sgr

#endif // AMBER_TUI_ANSI_SGR_H
