#include "agent/skill_ledger.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <system_error>

namespace agent {

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

std::string text(const json& j, const char* key) {
    if (!j.contains(key) || !j[key].is_string())
        return {};
    return j[key].get<std::string>();
}

LedgerEntry entry_from(const json& j) {
    LedgerEntry e;
    e.id = text(j, "id");
    e.source = text(j, "source");
    e.slug = text(j, "slug");
    e.ref = text(j, "ref");
    e.hash = text(j, "hash");
    e.category = text(j, "category");
    e.note = text(j, "note");
    e.installed_at_ms = j.value("installed_at_ms", 0LL);
    e.files = j.value("files", 0);
    e.bytes = j.value("bytes", 0LL);
    e.enabled = j.value("enabled", true);
    return e;
}

json entry_json(const LedgerEntry& e) {
    return {{"id", e.id},          {"source", e.source},
            {"slug", e.slug},      {"ref", e.ref},
            {"hash", e.hash},      {"category", e.category},
            {"note", e.note},      {"installed_at_ms", e.installed_at_ms},
            {"files", e.files},    {"bytes", e.bytes},
            {"enabled", e.enabled}};
}

} // namespace

SkillLedger::SkillLedger(std::string path) : path_(std::move(path)) {}

SkillLedger SkillLedger::for_root(const std::string& skills_root) {
    return SkillLedger(skills_root + "/.lock.json");
}

void SkillLedger::load() {
    entries_.clear();
    std::ifstream f(path_, std::ios::binary);
    if (!f)
        return;
    const json j = json::parse(f, nullptr, false);
    if (j.is_discarded() || !j.is_object())
        return;
    const auto it = j.find("entries");
    if (it == j.end() || !it->is_object())
        return;
    for (const auto& [name, value] : it->items()) {
        if (value.is_object())
            entries_[name] = entry_from(value);
    }
}

bool SkillLedger::save() const {
    json entries = json::object();
    for (const auto& [name, entry] : entries_)
        entries[name] = entry_json(entry);
    const json doc{{"version", 1}, {"entries", std::move(entries)}};

    std::error_code ec;
    fs::create_directories(fs::path(path_).parent_path(), ec);
    if (ec)
        return false;

    const std::string tmp = path_ + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f)
            return false;
        f << doc.dump(2);
        if (!f)
            return false;
    }
    fs::rename(tmp, path_, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

std::optional<LedgerEntry> SkillLedger::find(const std::string& name) const {
    const auto it = entries_.find(name);
    if (it == entries_.end())
        return std::nullopt;
    return it->second;
}

bool SkillLedger::upsert(const std::string& name, LedgerEntry entry) {
    entries_[name] = std::move(entry);
    return save();
}

bool SkillLedger::remove(const std::string& name) {
    if (entries_.erase(name) == 0)
        return true;
    return save();
}

} // namespace agent
