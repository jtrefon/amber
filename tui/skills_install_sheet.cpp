#include "tui/skills_install_sheet.h"

#include "tui/skills_format.h"

#include <algorithm>

namespace tui {

namespace {

bool vetted(agent::TrustTier tier) {
    return tier == agent::TrustTier::Official || tier == agent::TrustTier::Audited;
}

} // namespace

std::string human_bytes(long long bytes) {
    return human_size(bytes);
}

InstallSheet::InstallSheet(InstallEvidence evidence, InstallSheetOptions options)
    : evidence_(std::move(evidence)), options_(options) {
    if (options_.default_global)
        scope_ = InstallScope::Global;
    replace_ = collision();
}

bool InstallSheet::collision() const noexcept {
    return scope_ == InstallScope::Project ? options_.project_exists : options_.global_exists;
}

bool InstallSheet::review_satisfied() const noexcept {
    return !options_.review_required || vetted(evidence_.tier) || reviewed_ || unread_install_;
}

bool InstallSheet::audit_satisfied() const noexcept {
    return !options_.audit_required || evidence_.audited || audit_override_;
}

bool InstallSheet::can_confirm() const noexcept {
    if (collision() && !replace_)
        return false;
    return review_satisfied() && audit_satisfied();
}

std::string InstallSheet::target_root() const {
    return scope_ == InstallScope::Project ? ".amber/skills" : "~/.config/amber/skills";
}

std::string InstallSheet::target() const {
    if (evidence_.name.empty())
        return target_root();
    return target_root() + "/" + evidence_.name;
}

std::string InstallSheet::status() const {
    if (!audit_satisfied())
        return "audit required: this source is not audited -- press a to install anyway";
    if (!review_satisfied())
        return "review required: p reads the body, x installs it unread";
    if (collision() && !replace_)
        return "name collision at " + target() + " -- press r to replace, tab for the other scope";
    return "Enter installs into " + target() + " (" + std::to_string(evidence_.files) + " files, " +
           human_bytes(evidence_.bytes) + ")";
}

void InstallSheet::toggle_scope() {
    scope_ = scope_ == InstallScope::Project ? InstallScope::Global : InstallScope::Project;
    // The collision belongs to the scope, so the replace choice follows it.
    replace_ = collision();
}

void InstallSheet::mark_reviewed() {
    reviewed_ = true;
}

InstallSheetAction InstallSheet::key(int ch) {
    switch (ch) {
    case '\t':
    case 's':
        toggle_scope();
        return InstallSheetAction::ToggleScope;
    case 'e':
        enabled_ = !enabled_;
        return InstallSheetAction::ToggleEnabled;
    case 'x':
        unread_install_ = !unread_install_;
        return InstallSheetAction::ToggleReview;
    case 'r':
        replace_ = !replace_;
        return InstallSheetAction::ToggleReplace;
    case 'a':
        audit_override_ = !audit_override_;
        return InstallSheetAction::ToggleAudit;
    case 'p':
        mark_reviewed();
        return InstallSheetAction::Preview;
    case '\n':
    case '\r':
        return can_confirm() ? InstallSheetAction::Confirm : InstallSheetAction::None;
    case 27:
    case 'q':
        return InstallSheetAction::Cancel;
    default:
        return InstallSheetAction::None;
    }
}

InstallDecision InstallSheet::decision() const {
    InstallDecision decision;
    decision.confirmed = can_confirm();
    decision.scope = scope_;
    decision.replace = collision() && replace_;
    decision.enabled = enabled_;
    return decision;
}

} // namespace tui
