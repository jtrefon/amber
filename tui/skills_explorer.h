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
    // Mirror/self-hosted endpoints; empty keeps the public defaults.
    std::string index_base;
    std::string github_api_base;
    std::string github_raw_base;
    std::function<std::vector<SkillRow>()> installed;
    std::function<std::optional<agent::LedgerEntry>(const std::string&)> ledger;
    std::function<bool(const std::string&, const std::string&)> set_state;
    // True when the conversation has activated this skill. Removal names it,
    // because an activated body already sits in the prompt copy.
    std::function<bool(const std::string&)> is_active;
    std::string budget; // installed-tab summary, e.g. "14 lines · ~480 tok/turn"
    SkillTab initial_tab = SkillTab::Top;
    std::string initial_query;  // prefill a registry search (`/skills search`)
    std::string initial_filter; // prefill the local filter (`/skills remove`)
    std::string install_id;     // open the install sheet for this id (`/skills install`)
    bool install_global = false;
    bool review_required = true;        // `skills_review_required`
    bool update_review_required = true; // `skills_update_review`
    bool update_check_disabled = false; // `skills_update_check = never`
    bool audit_required = false;        // `skills_audit_required`
    bool index_disabled = false;        // `skills_index = none`
    std::string github_token;           // separate from the index token when configured
};

// Run the modal explorer until Esc/q. `status` receives one line per outcome
// (installed, failed, not available yet) so the caller can print it after the
// screen is restored.
void run_skills_explorer(const SkillsExplorerOptions& options,
                         const std::function<void(const std::string&)>& status);

} // namespace tui

#endif // AMBER_TUI_SKILLS_EXPLORER_H
