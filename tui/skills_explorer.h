#ifndef AMBER_TUI_SKILLS_EXPLORER_H
#define AMBER_TUI_SKILLS_EXPLORER_H

#include "agent/skill_ledger.h"
#include "tui/skills_browse_core.h"

#include <functional>
#include <string>
#include <vector>

namespace tui {

// What the host knows and the explorer must not discover for itself: where the
// two skill roots are, how to read the installed set, and how to report a line
// to the transcript after the modal closes.
struct SkillsExplorerOptions {
    int rows = 24;
    int cols = 80;
    std::string project_root; // <project>/.amber/skills
    std::string global_root;  // ~/.config/amber/skills
    std::string cache_root;
    std::string token;
    std::function<std::vector<SkillRow>()> installed;
    std::function<std::optional<agent::LedgerEntry>(const std::string&)> ledger;
    std::function<bool(const std::string&, const std::string&)> set_state;
    std::string budget; // installed-tab summary, e.g. "14 lines · ~480 tok/turn"
};

// Run the modal explorer until Esc/q. `status` receives one line per outcome
// (installed, failed, not available yet) so the caller can print it after the
// screen is restored.
void run_skills_explorer(const SkillsExplorerOptions& options,
                         const std::function<void(const std::string&)>& status);

} // namespace tui

#endif // AMBER_TUI_SKILLS_EXPLORER_H
