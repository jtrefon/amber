#ifndef AGENT_SKILL_LEDGER_H
#define AGENT_SKILL_LEDGER_H

#include <map>
#include <optional>
#include <string>

namespace agent {

// Where an installed skill came from. Written beside the skill directories
// (`.lock.json`) so provenance travels with a shared project tree instead of
// living in a per-machine database that a clone loses.
struct LedgerEntry {
    std::string id;     // registry id: source/slug
    std::string source; // owner/repo or host
    std::string slug;
    std::string ref;        // branch or tag installed from
    std::string hash;       // remote SKILL.md blob SHA at install time
    std::string local_hash; // FNV of the installed SKILL.md (local-edit check)
    std::string category;
    std::string note;
    long long installed_at_ms = 0;
    int files = 0;
    long long bytes = 0;
    bool enabled = true;
};

// Read/write `.lock.json` in one skills root. Tolerant on read (a corrupt or
// older file is empty, never fatal); atomic on write (temp + rename), so an
// interrupted write cannot truncate the ledger.
class SkillLedger {
public:
    explicit SkillLedger(std::string path);

    // `<skills_root>/.lock.json`.
    static SkillLedger for_root(const std::string& skills_root);

    const std::string& path() const noexcept { return path_; }

    void load();
    bool save() const;

    std::optional<LedgerEntry> find(const std::string& name) const;
    const std::map<std::string, LedgerEntry>& entries() const noexcept { return entries_; }

    // Both persist immediately. False means the file could not be written; the
    // in-memory map is still updated so the session does not lie about itself.
    bool upsert(const std::string& name, LedgerEntry entry);
    bool remove(const std::string& name);

private:
    std::string path_;
    std::map<std::string, LedgerEntry> entries_;
};

} // namespace agent

#endif // AGENT_SKILL_LEDGER_H
