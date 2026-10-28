#include "tui/skills_lifecycle.h"

#include "agent/skill_install.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace tui {

namespace {

std::string read_text(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return {};
    return {(std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()};
}

} // namespace

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
    const std::string dir = root + "/" + row.ref.name;
    agent::UpdateReview review = agent::build_update_review(
        *entry, detail, read_text(dir + "/SKILL.md"), agent::installed_skill_files(dir));
    details_[entry->id] = detail;
    updates_.insert(row.ref.name);
    update_ready_.insert(row.ref.name);

    std::string status = review.status;
    if (review.needs_override())
        status += " · local edits: d reviews, u overrides";
    reviews_[row.ref.name] = std::move(review);
    return status;
}

std::optional<agent::UpdateReview> SkillsLifecycle::review_for(const std::string& name) const {
    const auto it = reviews_.find(name);
    if (it == reviews_.end())
        return std::nullopt;
    return it->second;
}

std::string SkillsLifecycle::apply_update(const SkillRow& row, bool force) {
    const auto entry = entry_for(row.ref.name);
    if (!entry)
        return row.ref.name + " is unmanaged";
    const std::string root = root_for(row);
    if (root.empty())
        return "unknown install root for " + row.ref.name;

    const auto detail = details_.find(entry->id);
    if (detail == details_.end())
        return "review " + row.ref.name + " first (u)";
    const auto review = reviews_.find(row.ref.name);
    if (review == reviews_.end())
        return "review " + row.ref.name + " first (u)";
    if (review->second.needs_override() && !force)
        return row.ref.name + " has local edits; d reviews, u again overrides";

    service_.install(detail->second, row.ref.name, root, true, entry->enabled);
    update_ready_.erase(row.ref.name);
    reviews_.erase(row.ref.name);
    return "updating " + row.ref.name + "...";
}

std::string SkillsLifecycle::apply_all(const std::vector<SkillRow>& rows) {
    std::vector<std::string> ready(updates_.begin(), updates_.end());

    int applied = 0;
    std::vector<std::string> skipped;
    skipped.reserve(ready.size());
    for (const auto& name : ready) {
        const auto row = std::find_if(rows.begin(), rows.end(),
                                      [&name](const SkillRow& r) { return r.ref.name == name; });
        if (row == rows.end())
            continue;
        const auto review = reviews_.find(name);
        if (review != reviews_.end() && review->second.needs_override()) {
            skipped.emplace_back(name + " (local edits)");
            continue;
        }
        const std::string status = apply_update(*row, false);
        if (status.rfind("updating ", 0) == 0) {
            ++applied;
            continue;
        }
        std::string entry = name;
        entry.append(" (").append(status).append(")");
        skipped.push_back(std::move(entry));
    }

    std::string summary = std::to_string(applied) + " updated";
    if (!skipped.empty()) {
        summary += ", " + std::to_string(skipped.size()) + " skipped: ";
        for (std::size_t i = 0; i < skipped.size(); ++i) {
            if (i > 0)
                summary += ", ";
            summary += skipped[i];
        }
    }
    return summary;
}

void SkillsLifecycle::on_install(const std::string& name) {
    updates_.erase(name);
    update_ready_.erase(name);
    reviews_.erase(name);
    for (auto it = details_.begin(); it != details_.end();) {
        if (it->second.ref.name == name)
            it = details_.erase(it);
        else
            ++it;
    }
}

} // namespace tui
