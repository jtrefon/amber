#ifndef AMBER_TUI_SKILLS_INSTALL_SHEET_H
#define AMBER_TUI_SKILLS_INSTALL_SHEET_H

#include "agent/remote_text.h"

#include <string>
#include <vector>

namespace tui {

// Compact size for the sheet and the success line ("42 KB"), shared so the two
// surfaces cannot disagree.
std::string human_bytes(long long bytes);

enum class InstallScope : unsigned char { Project, Global };

// What a key asks the sheet to do. The sheet never performs I/O: the view
// renders it, and the explorer acts on the confirmation.
enum class InstallSheetAction : unsigned char {
    None,
    ToggleScope,
    ToggleEnabled,
    ToggleReview,
    ToggleReplace,
    ToggleAudit,
    Preview,
    Confirm,
    Cancel,
};

// The evidence the sheet shows. Populated from the fetched detail; nothing here
// is re-declared as fact -- "not audited" is a claim about the source, not a
// verdict on the skill.
struct InstallEvidence {
    std::string name;
    std::string source;
    std::string category;
    std::string license;
    agent::TrustTier tier = agent::TrustTier::Unverified;
    std::vector<agent::RiskSignal> risks;
    int files = 0;
    long long bytes = 0;
    int tokens = 0;
    bool audited = false;
};

struct InstallSheetOptions {
    bool project_exists = false;
    bool global_exists = false;
    // `skills_review_required` (default on): an unvetted source cannot be
    // installed until its body has been shown, or the user explicitly opts to
    // install it unread.
    bool review_required = true;
    // `skills_audit_required` (default off until a source provides audits).
    bool audit_required = false;
};

struct InstallDecision {
    bool confirmed = false;
    InstallScope scope = InstallScope::Project;
    bool replace = false;
    bool enabled = true;
};

// The install sheet's state machine, with no ncurses and no I/O. Every gate is
// a named predicate so the view can explain why Enter is inert instead of
// rendering a dead-looking button.
class InstallSheet {
public:
    InstallSheet(InstallEvidence evidence, InstallSheetOptions options);

    InstallSheetAction key(int ch);

    const InstallEvidence& evidence() const noexcept { return evidence_; }
    InstallScope scope() const noexcept { return scope_; }
    bool enabled() const noexcept { return enabled_; }
    bool reviewed() const noexcept { return reviewed_; }
    bool unread_install() const noexcept { return unread_install_; }
    bool replace() const noexcept { return replace_; }
    bool audit_override() const noexcept { return audit_override_; }
    bool collision() const noexcept;
    bool review_satisfied() const noexcept;
    bool audit_satisfied() const noexcept;
    bool can_confirm() const noexcept;

    // True when the current scope already holds a directory of this name.
    std::string target_root() const;
    std::string target() const;

    // A line explaining the current gate, or what Enter will do.
    std::string status() const;

    InstallDecision decision() const;

private:
    void toggle_scope();
    void mark_reviewed();

    InstallEvidence evidence_;
    InstallSheetOptions options_;
    InstallScope scope_ = InstallScope::Project;
    bool enabled_ = true;
    bool reviewed_ = false;
    bool unread_install_ = false;
    bool audit_override_ = false;
    bool replace_ = false;
};

} // namespace tui

#endif // AMBER_TUI_SKILLS_INSTALL_SHEET_H
