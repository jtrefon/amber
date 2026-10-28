#ifndef AMBER_AGENT_SKILL_UPDATE_REVIEW_H
#define AMBER_AGENT_SKILL_UPDATE_REVIEW_H

#include "agent/skill_index.h"
#include "agent/skill_ledger.h"
#include "agent/text_diff.h"

#include <string>
#include <vector>

namespace agent {

// What an update would change, assembled before anything is written. Pure: the
// caller reads the installed tree and the remote detail, this decides what the
// review says. An update is never applied from the hash alone.
struct UpdateReview {
    std::string name;
    std::string from_hash; // ledger hash at install time
    std::string to_hash;   // remote hash now
    std::string status;    // one line for the explorer footer / CLI text
    std::vector<std::string> body_diff;
    std::vector<std::string> added_files;
    std::vector<std::string> removed_files;
    DiffCounts body_counts;
    bool body_changed = false;
    // The installed SKILL.md no longer matches the hash the ledger recorded, so
    // someone edited it after install. Applying would discard that work.
    bool local_modified = false;

    bool has_changes() const noexcept {
        return body_changed || !added_files.empty() || !removed_files.empty();
    }
    bool needs_override() const noexcept { return local_modified; }
};

UpdateReview build_update_review(const LedgerEntry& entry, const SkillDetail& detail,
                                 const std::string& installed_body,
                                 const std::vector<std::string>& installed_files);

// The pager body: a header naming both sides, the local-edit warning when it
// applies, the SKILL.md diff, then added/removed files.
std::vector<std::string> update_review_lines(const UpdateReview& review);

// Regular files under one installed skill directory, relative to it and sorted.
// SKILL.md is included; the review separates it from the rest.
std::vector<std::string> installed_skill_files(const std::string& dir);

} // namespace agent

#endif // AMBER_AGENT_SKILL_UPDATE_REVIEW_H
