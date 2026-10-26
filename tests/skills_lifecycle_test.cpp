#include "tui/skills_lifecycle.h"
#include "agent/skill_install.h"
#include "agent/skill_ledger.h"
#include "tests/test_util.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct TempDir {
    explicit TempDir(const std::string& name) : path("/tmp/amber_lifecycle_" + name) {
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    std::string path;
};

const char* kSkillMd = "---\nname: handwritten\ndescription: Local.\n---\nbody\n";

void write(const std::string& path, const std::string& contents) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream f(path, std::ios::binary);
    f << contents;
}

std::string read(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

tui::SkillRow row_for(const std::string& root, const std::string& name,
                      const std::string& state = "enabled") {
    tui::SkillRow row;
    row.ref.id = root + "/" + name;
    row.ref.name = name;
    row.ref.source = "project";
    row.installed = true;
    row.state = state;
    return row;
}

agent::LedgerEntry entry_for(const std::string& name, const std::string& remote_hash,
                             const std::string& local_hash) {
    agent::LedgerEntry e;
    e.id = "acme/skills/" + name;
    e.source = "acme/skills";
    e.slug = name;
    e.ref = "main";
    e.hash = remote_hash;
    e.local_hash = local_hash;
    e.enabled = true;
    return e;
}

void put_ledger(const std::string& root, const std::string& name, agent::LedgerEntry entry) {
    agent::SkillLedger ledger = agent::SkillLedger::for_root(root);
    ledger.load();
    ledger.upsert(name, std::move(entry));
}

tui::SkillsLifecycleOptions options_for(const std::string& root,
                                        std::vector<std::pair<std::string, std::string>>* calls) {
    tui::SkillsLifecycleOptions options;
    options.project_root = root;
    options.ledger = [root](const std::string& name) {
        agent::SkillLedger ledger = agent::SkillLedger::for_root(root);
        ledger.load();
        return ledger.find(name);
    };
    if (calls) {
        options.set_state = [calls](const std::string& name, const std::string& state) {
            calls->push_back({name, state});
            return true;
        };
    }
    return options;
}

agent::SkillDetail detail_for(const std::string& name, const std::string& hash) {
    agent::SkillDetail detail;
    detail.ref.id = "acme/skills/" + name;
    detail.ref.name = name;
    detail.ref.source = "acme/skills";
    detail.hash = hash;
    detail.default_branch = "main";
    return detail;
}

} // namespace

TEST(lifecycle_adopt_writes_provenance_without_rewriting) {
    TempDir tmp("adopt");
    write(tmp.path + "/mine/SKILL.md", kSkillMd);
    write(tmp.path + "/mine/notes.md", "notes");
    tui::SkillsService service(tui::SkillsBackends{});
    tui::SkillsLifecycle life(options_for(tmp.path, nullptr), service);

    const auto row = row_for(tmp.path, "mine");
    ASSERT_EQ(life.adopt(row), std::string("adopted mine"));
    ASSERT_EQ(read(tmp.path + "/mine/notes.md"), std::string("notes"));

    agent::SkillLedger ledger = agent::SkillLedger::for_root(tmp.path);
    ledger.load();
    const auto entry = ledger.find("mine");
    ASSERT_TRUE(entry.has_value());
    ASSERT_EQ(entry->local_hash, agent::skill_content_hash(kSkillMd));
    ASSERT_EQ(life.adopt(row), std::string("mine is already managed"));
}

TEST(lifecycle_remove_deletes_tree_and_entry) {
    TempDir tmp("remove");
    write(tmp.path + "/mine/SKILL.md", kSkillMd);
    tui::SkillsService service(tui::SkillsBackends{});
    tui::SkillsLifecycle life(options_for(tmp.path, nullptr), service);
    const auto row = row_for(tmp.path, "mine");
    ASSERT_EQ(life.adopt(row), std::string("adopted mine"));

    ASSERT_EQ(life.remove(row), std::string("removed mine"));
    ASSERT_TRUE(!fs::exists(tmp.path + "/mine"));
    agent::SkillLedger ledger = agent::SkillLedger::for_root(tmp.path);
    ledger.load();
    ASSERT_TRUE(!ledger.find("mine").has_value());
}

TEST(lifecycle_toggle_and_block_go_through_the_override) {
    TempDir tmp("toggle");
    std::vector<std::pair<std::string, std::string>> calls;
    tui::SkillsService service(tui::SkillsBackends{});
    tui::SkillsLifecycle life(options_for(tmp.path, &calls), service);

    ASSERT_EQ(life.toggle(row_for(tmp.path, "mine", "enabled")), std::string("mine disabled"));
    ASSERT_EQ(life.toggle(row_for(tmp.path, "mine", "disabled")), std::string("mine enabled"));
    ASSERT_EQ(life.block(row_for(tmp.path, "mine")), std::string("mine blocked"));
    ASSERT_EQ(static_cast<int>(calls.size()), 3);
    ASSERT_EQ(calls[0].second, std::string("disable"));
    ASSERT_EQ(calls[1].second, std::string("enable"));
    ASSERT_EQ(calls[2].second, std::string("block"));
}

TEST(lifecycle_update_check_reports_and_flags) {
    TempDir tmp("check");
    write(tmp.path + "/mine/SKILL.md", kSkillMd);
    const std::string local = agent::skill_content_hash(kSkillMd);
    put_ledger(tmp.path, "mine", entry_for("mine", "remote", local));

    tui::SkillsService service(tui::SkillsBackends{});
    tui::SkillsLifecycle life(options_for(tmp.path, nullptr), service);
    const auto row = row_for(tmp.path, "mine");

    ASSERT_EQ(life.check_update(row, detail_for("mine", "remote")),
              std::string("mine is up to date"));
    ASSERT_TRUE(!life.has_update("mine"));

    ASSERT_EQ(life.check_update(row, detail_for("mine", "new")),
              std::string("update available for mine (u applies)"));
    ASSERT_TRUE(life.has_update("mine"));
}

TEST(lifecycle_update_refuses_a_local_edit) {
    TempDir tmp("localedit");
    write(tmp.path + "/mine/SKILL.md", kSkillMd);
    put_ledger(tmp.path, "mine", entry_for("mine", "remote", "stale-hash"));

    tui::SkillsService service(tui::SkillsBackends{});
    tui::SkillsLifecycle life(options_for(tmp.path, nullptr), service);
    const auto row = row_for(tmp.path, "mine");

    ASSERT_EQ(life.check_update(row, detail_for("mine", "new")),
              std::string("mine has local edits; update refused"));
    ASSERT_TRUE(!life.has_update("mine"));
}

TEST(lifecycle_prepare_update_handles_unmanaged_fetch_and_apply) {
    TempDir tmp("prepare");
    write(tmp.path + "/mine/SKILL.md", kSkillMd);
    tui::SkillsService service(tui::SkillsBackends{});
    tui::SkillsLifecycle life(options_for(tmp.path, nullptr), service);
    const auto row = row_for(tmp.path, "mine");

    agent::SkillRef ref;
    std::string status;
    ASSERT_TRUE(life.prepare_update(row, ref, status) == tui::UpdateAction::None);
    ASSERT_TRUE(status.find("unmanaged") != std::string::npos);

    put_ledger(tmp.path, "mine", entry_for("mine", "remote", agent::skill_content_hash(kSkillMd)));
    ASSERT_TRUE(life.prepare_update(row, ref, status) == tui::UpdateAction::Fetch);
    ASSERT_EQ(ref.id, std::string("acme/skills/mine"));

    life.check_update(row, detail_for("mine", "new"));
    ASSERT_TRUE(life.prepare_update(row, ref, status) == tui::UpdateAction::Apply);

    ASSERT_TRUE(life.apply_update(row).find("updating") != std::string::npos);
    ASSERT_TRUE(life.has_update("mine")); // badge stays until the install event
    life.on_install("mine");
    ASSERT_TRUE(!life.has_update("mine"));
}

TEST(lifecycle_update_without_details_is_refused) {
    TempDir tmp("nodetail");
    write(tmp.path + "/mine/SKILL.md", kSkillMd);
    put_ledger(tmp.path, "mine", entry_for("mine", "remote", agent::skill_content_hash(kSkillMd)));
    tui::SkillsService service(tui::SkillsBackends{});
    tui::SkillsLifecycle life(options_for(tmp.path, nullptr), service);

    ASSERT_TRUE(life.apply_update(row_for(tmp.path, "mine")).find("details") != std::string::npos);
}
