#include "tui/skills_service.h"
#include "agent/skill_ledger.h"
#include "tests/test_util.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>

namespace {

namespace fs = std::filesystem;

struct TempDir {
    explicit TempDir(const std::string& name) : path("/tmp/amber_service_" + name) {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    std::string path;
};

agent::SkillRef tdd_ref() {
    agent::SkillRef ref;
    ref.id = "acme/skills/tdd";
    ref.name = "tdd";
    ref.source = "acme/skills";
    ref.installs = 10;
    return ref;
}

agent::SkillDetail tdd_detail() {
    agent::SkillDetail detail;
    detail.ref = tdd_ref();
    detail.description = "Test-driven development";
    detail.category = "engineering";
    detail.default_branch = "main";
    detail.hash = "aaa";
    return detail;
}

std::optional<tui::SkillsEvent> wait_for(tui::SkillsService& service, tui::SkillsEvent::Kind kind,
                                         int timeout_ms = 2000) {
    for (int i = 0; i < timeout_ms; ++i) {
        for (auto& event : service.drain()) {
            if (event.kind == kind)
                return event;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::nullopt;
}

const char* kSkillMd = "---\nname: tdd\ndescription: X\n---\nbody\n";

tui::SkillsBackends backends() {
    tui::SkillsBackends b;
    b.search = [](const std::string&, int, const agent::CancellationToken&) {
        agent::SkillPage page;
        page.skills.push_back(tdd_ref());
        page.total = 1;
        return agent::IndexResult{page, {}};
    };
    b.detail = [](const agent::SkillRef&, const agent::CancellationToken&) {
        return agent::DetailResult{tdd_detail(), {}};
    };
    b.assets = [](const agent::SkillDetail&, const agent::CancellationToken&) {
        std::vector<agent::SkillAssetContent> files{{"SKILL.md", kSkillMd}};
        return agent::AssetsResult{std::move(files), {}};
    };
    b.install = [](const agent::SkillInstallRequest& request) {
        return agent::install_skill_files(request);
    };
    return b;
}

} // namespace

TEST(skills_service_search_reports_rows) {
    tui::SkillsService service(backends());
    service.search("tdd");
    const auto event = wait_for(service, tui::SkillsEvent::Kind::SearchDone);
    ASSERT_TRUE(event.has_value());
    ASSERT_EQ(event->query, std::string("tdd"));
    ASSERT_TRUE(event->search.ok());
    ASSERT_EQ(static_cast<int>(event->search.page->skills.size()), 1);
}

TEST(skills_service_detail_reports_the_skill) {
    tui::SkillsService service(backends());
    service.detail(tdd_ref());
    const auto event = wait_for(service, tui::SkillsEvent::Kind::DetailDone);
    ASSERT_TRUE(event.has_value());
    ASSERT_TRUE(event->detail.ok());
    ASSERT_EQ(event->detail.detail->description, std::string("Test-driven development"));
}

TEST(skills_service_install_writes_files_and_the_ledger) {
    TempDir tmp("install");
    tui::SkillsService service(backends());
    service.install(tdd_detail(), "tdd", tmp.path, false, true);
    const auto event = wait_for(service, tui::SkillsEvent::Kind::InstallDone);
    ASSERT_TRUE(event.has_value());
    ASSERT_TRUE(event->install.ok);
    ASSERT_TRUE(event->ledger_saved);
    ASSERT_TRUE(fs::exists(tmp.path + "/tdd/SKILL.md"));

    agent::SkillLedger ledger = agent::SkillLedger::for_root(tmp.path);
    ledger.load();
    const auto entry = ledger.find("tdd");
    ASSERT_TRUE(entry.has_value());
    ASSERT_EQ(entry->source, std::string("acme/skills"));
    ASSERT_EQ(entry->hash, std::string("aaa"));
    ASSERT_TRUE(entry->enabled);
}

TEST(skills_service_install_failure_is_reported) {
    auto b = backends();
    b.assets = [](const agent::SkillDetail&, const agent::CancellationToken&) {
        return agent::AssetsResult{std::nullopt, {agent::IndexError::Offline, 0, 0, "network"}};
    };
    TempDir tmp("install_fail");
    tui::SkillsService service(std::move(b));
    service.install(tdd_detail(), "tdd", tmp.path, false, true);
    const auto event = wait_for(service, tui::SkillsEvent::Kind::InstallDone);
    ASSERT_TRUE(event.has_value());
    ASSERT_TRUE(!event->install.ok);
    ASSERT_TRUE(event->install.error.find("network") != std::string::npos);
    ASSERT_TRUE(!fs::exists(tmp.path + "/tdd"));
}

TEST(skills_service_cancel_stops_an_in_flight_search) {
    std::atomic<bool> started{false};
    auto b = backends();
    b.search = [&started](const std::string&, int, const agent::CancellationToken& cancel) {
        started = true;
        while (!cancel.is_requested())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return agent::IndexResult{std::nullopt, {agent::IndexError::Cancelled, 0, 0, "cancelled"}};
    };

    tui::SkillsService service(std::move(b));
    service.search("tdd");
    for (int i = 0; i < 2000 && !started; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_TRUE(started);
    service.cancel();

    const auto event = wait_for(service, tui::SkillsEvent::Kind::SearchDone);
    ASSERT_TRUE(event.has_value());
    ASSERT_TRUE(!event->search.ok());
    ASSERT_TRUE(event->search.error.kind == agent::IndexError::Cancelled);
}
