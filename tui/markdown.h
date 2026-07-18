// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AMBER_TUI_MARKDOWN_H
#define AMBER_TUI_MARKDOWN_H

#include <string>
#include <vector>

#include "rich.h"

namespace tui {
namespace md {

// Tuning for the Markdown -> RichLine renderer. Colors come from the shared
// widgets.h pair ids so the chat stays consistent with the rest of the TUI.
 struct Style {
     int text_pair = P_ASSISTANT;     // body / paragraph text
     int heading_pair = P_MD_HEAD;    // headings
     int code_pair = P_MD_CODE;       // inline + fenced code
     int quote_pair = P_MD_QUOTE;     // block quotes
     int emph_pair = P_ASSISTANT;     // bold/strong emphasis (bold attr added)
     int link_pair = P_MD_LINK;       // link text
     int table_pair = P_MD_TABLE;     // table body
     int table_head_pair = P_MD_HEAD; // table header row
     int hr_pair = P_MD_HR;           // horizontal rule
 };

// Render a Markdown document (which may also contain inline ANSI SGR escapes
// emitted by the model) into a list of RichLines ready for the canvas. ANSI SGR
// is mapped onto Run styles/colors rather than stripped.
std::vector<rich::Line> render(const std::string& md, const Style& st = Style{});

// Lightweight heuristic source highlighter for fenced code. Returns RichLines
// (one per source line) with comment/string/number/keyword runs colored.
// Not language-perfect; aims for pleasant structure on common languages.
std::vector<rich::Line> highlight(const std::string& code,
                                  const std::string& lang, int code_pair);

} // namespace md
} // namespace tui

#endif // AMBER_TUI_MARKDOWN_H
