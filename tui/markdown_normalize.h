#ifndef AMBER_TUI_MARKDOWN_NORMALIZE_H
#define AMBER_TUI_MARKDOWN_NORMALIZE_H

#include <string>

// L1 (pure): heuristically repair the near-markdown LLMs emit — GFM tables with
// no blank line before them (md4c requires one), tables missing their delimiter
// row, ragged tables whose header has fewer columns than the body, and stray
// separator runs (model-emitted "─------─" rules) glued onto a paragraph.
// Returns markdown md4c can parse into proper tables / horizontal rules.
// No ncurses, no parser state.
namespace tui::markdown_normalize {

std::string normalize(const std::string& md);

} // namespace tui::markdown_normalize

#endif // AMBER_TUI_MARKDOWN_NORMALIZE_H
