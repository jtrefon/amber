#ifndef AMBER_TUI_SKILLS_SHEET_VIEW_H
#define AMBER_TUI_SKILLS_SHEET_VIEW_H

#include "tui/skills_install_sheet.h"

#include <string>

namespace tui {

// Render the install sheet and block until the user confirms or cancels.
// `body` is the sanitized SKILL.md, shown by `p` (the review gate) before the
// sheet lets Enter install an unvetted source.
InstallDecision run_install_sheet(const InstallEvidence& evidence, const std::string& body,
                                  const InstallSheetOptions& options);

// A read-only pager for a skill body. Exposed so the explorer's detail pane can
// reuse the same surface instead of growing a second scroller.
void run_body_preview(const std::string& title, const std::string& body);

} // namespace tui

#endif // AMBER_TUI_SKILLS_SHEET_VIEW_H
