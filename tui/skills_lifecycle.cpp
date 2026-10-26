#include "tui/skills_lifecycle.h"

#include "agent/skill_install.h"

#include <filesystem>
#include <fstream>
#include <iterator>

namespace tui {

SkillsLifecycle::SkillsLifecycle(SkillsLifecycleOptions options, SkillsService& service)
    : options_(std::move(options)), service_(service) {}

void SkillsLifecycle::set_installed(std::vector<SkillRow> rows) {
    installed_ = std::move(rows);
}

std::string SkillsLifecycle::root_for(const SkillRow& row) const {
    // Installed rows carry their directory path as the id; deriving the root
    // from it is exact even when a scope label and a configured path disagree.
    if (row.ref.id.find('/') != std::string::npos) {
        const std::filesystem::path path(row.ref.id);
        if (path.has_parent_path())
            return path.parent_path().string();
    }
    if (row.ref.source == "project")
        return options_.project_root;
    if (row.ref.source == "global")
        return options_.global_root;
    return {};
}

std::string SkillsLifecycle::install_root(const SkillRow& row) const {
    return root_for(row);
}

std::optional<agent::LedgerEntry> SkillsLifecycle::entry_for(const std::string& name) const {
    return options_.ledger ? options_.ledger(name) : std::nullopt;
}

std::string SkillsLifecycle::local_hash(const std::string& root, const std::string& name) const {
    std::ifstream f(root + "/" + name + "/SKILL.md", std::ios::binary);
    if (!f)
        return {};
    const std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return agent::skill_content_hash(body);
}

std::string SkillsLifecycle::adopt(const SkillRow& row) {
    if (!row.installed)
        return "only installed skills can be adopted";
    if (entry_for(row.ref.name))
        return row.ref.name + " is already managed";
    const std::string root = root_for(row);
    if (root.empty())
        return "only project/global skills can be adopted";

    const auto entry = agent::adopt_skill(root, row.ref.name);
    if (!entry)
        return "cannot adopt " + row.ref.name;
    agent::SkillLedger ledger = agent::SkillLedger::for_root(root);
    ledger.load();
    ledger.upsert(row.ref.name, *entry);
    return "adopted " + row.ref.name;
}

std::string SkillsLifecycle::remove(const SkillRow& row) {
    if (!row.installed)
        return "only installed skills can be removed";
    const std::string root = root_for(row);
    if (root.empty())
        return "only project/global skills can be removed here";

    std::string error = agent::uninstall_skill(row.ref.name, root);
    if (!error.empty())
        return error;
    agent::SkillLedger ledger = agent::SkillLedger::for_root(root);
    ledger.load();
    ledger.remove(row.ref.name);
    updates_.erase(row.ref.name);
    update_ready_.erase(row.ref.name);
    for (auto it = details_.begin(); it != details_.end();) {
        if (it->second.ref.name == row.ref.name)
            it = details_.erase(it);
        else
            ++it;
    }
    return "removed " + row.ref.name;
}

std::string SkillsLifecycle::toggle(const SkillRow& row) {
    if (!row.installed)
        return "only an installed skill can be toggled";
    const bool off = row.state == "disabled" || row.state == "blocked";
    if (!options_.set_state || !options_.set_state(row.ref.name, off ? "enable" : "disable"))
        return "cannot change " + row.ref.name;
    return row.ref.name + (off ? " enabled" : " disabled");
}

std::string SkillsLifecycle::block(const SkillRow& row) {
    if (!row.installed)
        return "only an installed skill can be blocked";
    if (!options_.set_state || !options_.set_state(row.ref.name, "block"))
        return "cannot block " + row.ref.name;
    return row.ref.name + " blocked";
}

UpdateAction SkillsLifecycle::prepare_update(const SkillRow& row, agent::SkillRef& ref,
                                             std::string& status) {
    if (!row.installed) {
        status = "only an installed skill can be updated";
        return UpdateAction::None;
    }
    const auto entry = entry_for(row.ref.name);
    if (!entry) {
        status = row.ref.name + " is unmanaged (press A to adopt)";
        return UpdateAction::None;
    }
    if (update_ready_.count(row.ref.name) != 0)
        return UpdateAction::Apply;

    ref.id = entry->id;
    ref.source = entry->source;
    ref.name = row.ref.name;
    ref.installs = row.ref.installs;
    status = "checking " + row.ref.name + "...";
    return UpdateAction::Fetch;
}

std::string SkillsLifecycle::check_update(const SkillRow& row, const agent::SkillDetail& detail) {
    const auto entry = entry_for(row.ref.name);
    if (!entry || entry->id != detail.ref.id)
        return "no provenance recorded for " + row.ref.name;
    if (detail.hash == entry->hash)
        return row.ref.name + " is up to date";

    const std::string root = root_for(row);
    if (root.empty())
        return "unknown install root for " + row.ref.name;
    if (local_hash(root, row.ref.name) != entry->local_hash)
        return row.ref.name + " has local edits; update refused";

    details_[entry->id] = detail;
    updates_.insert(row.ref.name);
    update_ready_.insert(row.ref.name);
    return "update available for " + row.ref.name + " (u applies)";
}

std::string SkillsLifecycle::apply_update(const SkillRow& row) {
    const auto entry = entry_for(row.ref.name);
    if (!entry)
        return row.ref.name + " is unmanaged";
    const std::string root = root_for(row);
    if (root.empty())
        return "unknown install root for " + row.ref.name;

    const auto detail = details_.find(entry->id);
    if (detail == details_.end())
        return "load " + row.ref.name + " details first";
    if (local_hash(root, row.ref.name) != entry->local_hash) {
        updates_.erase(row.ref.name);
        update_ready_.erase(row.ref.name);
        return row.ref.name + " has local edits; update refused";
    }
    service_.install(detail->second, row.ref.name, root, true, entry->enabled);
    update_ready_.erase(row.ref.name);
    return "updating " + row.ref.name + "...";
}

void SkillsLifecycle::on_install(const std::string& name) {
    updates_.erase(name);
    update_ready_.erase(name);
    for (auto it = details_.begin(); it != details_.end();) {
        if (it->second.ref.name == name)
            it = details_.erase(it);
        else
            ++it;
    }
}

} // namespace tui
