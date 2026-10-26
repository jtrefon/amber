#ifndef AMBER_TUI_SKILLS_LIFECYCLE_H
#define AMBER_TUI_SKILLS_LIFECYCLE_H

#include "agent/skill_index.h"
#include "agent/skill_ledger.h"
#include "tui/skills_browse_core.h"
#include "tui/skills_service.h"

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace tui {

struct SkillsLifecycleOptions {
    std::string project_root;
    std::string global_root;
    std::function<std::optional<agent::LedgerEntry>(const std::string&)> ledger;
    std::function<bool(const std::string&, const std::string&)> set_state;
};

enum class UpdateAction : unsigned char { None, Fetch, Apply };

// The decisions behind the explorer's lifecycle keys, with no ncurses and no
// direct network: adopt/remove/toggle/block are local, and an update is split
// into "fetch the remote detail" (the host calls the service), "check it" and
// "apply it". That split is what makes the behaviour testable.
class SkillsLifecycle {
public:
    SkillsLifecycle(SkillsLifecycleOptions options, SkillsService& service);

    void set_installed(std::vector<SkillRow> rows);

    // The directory that holds this skill; empty when the scope is not
    // project/global (interop/system are read-only here).
    std::string install_root(const SkillRow& row) const;

    std::string adopt(const SkillRow& row);
    std::string remove(const SkillRow& row);
    std::string toggle(const SkillRow& row);
    std::string block(const SkillRow& row);

    // Either asks the host to fetch `ref`, or says the cached detail is ready
    // to apply. `status` is always a line the explorer can show.
    UpdateAction prepare_update(const SkillRow& row, agent::SkillRef& ref, std::string& status);
    std::string check_update(const SkillRow& row, const agent::SkillDetail& detail);
    std::string apply_update(const SkillRow& row);

    void on_install(const std::string& name);
    bool has_update(const std::string& name) const { return updates_.count(name) != 0; }
    const std::set<std::string>& updates() const noexcept { return updates_; }

private:
    std::optional<agent::LedgerEntry> entry_for(const std::string& name) const;
    std::string local_hash(const std::string& root, const std::string& name) const;
    std::string root_for(const SkillRow& row) const;

    SkillsLifecycleOptions options_;
    SkillsService& service_;
    std::vector<SkillRow> installed_;
    std::set<std::string> updates_;
    std::set<std::string> update_ready_;
    std::map<std::string, agent::SkillDetail> details_;
};

} // namespace tui

#endif // AMBER_TUI_SKILLS_LIFECYCLE_H
