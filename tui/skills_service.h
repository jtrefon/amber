#ifndef AMBER_TUI_SKILLS_SERVICE_H
#define AMBER_TUI_SKILLS_SERVICE_H

#include "agent/github_index.h"
#include "agent/skill_index.h"
#include "agent/skill_install.h"
#include "agent/skill_ledger.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace tui {

// The three answers the explorer needs, behind plain functions so a test can
// run the service without a socket and the TUI never sees an adapter.
struct SkillsBackends {
    std::function<agent::IndexResult(const std::string&, int, const agent::CancellationToken&)>
        search;
    std::function<agent::DetailResult(const agent::SkillRef&, const agent::CancellationToken&)>
        detail;
    std::function<agent::AssetsResult(const agent::SkillDetail&, const agent::CancellationToken&)>
        assets;
    std::function<agent::SkillInstallResult(const agent::SkillInstallRequest&)> install;
};

struct SkillsEvent {
    enum class Kind : unsigned char { SearchDone, DetailDone, FilesDone, InstallDone };

    Kind kind = Kind::SearchDone;
    std::string query;
    agent::IndexResult search;
    agent::DetailResult detail;
    agent::SkillInstallResult install;
    // The files of one skill, in answer to a `files()` request. Kept apart from
    // the detail because a preview of a second file should not re-fetch the
    // body the detail already carries.
    agent::AssetsResult files;
    std::string detail_id;
    std::string name;
    bool ledger_saved = false;
};

// One worker thread owns every call. The UI thread posts a job and drains
// events; it never waits on I/O. A new search cancels the previous one and
// drops queued stale searches, so a burst of keystrokes cannot queue a burst of
// requests.
class SkillsService {
public:
    explicit SkillsService(SkillsBackends backends);
    ~SkillsService();

    SkillsService(const SkillsService&) = delete;
    SkillsService& operator=(const SkillsService&) = delete;

    void search(std::string query, int limit = 30);
    void detail(agent::SkillRef ref);
    // Every file's bytes for one skill, on demand (a preview of a file other
    // than SKILL.md).
    void files(agent::SkillDetail detail);
    void install(agent::SkillDetail detail, std::string name, std::string root, bool replace,
                 bool enabled);

    // Cancel the running search and install (closing the explorer).
    void cancel();

    // UI thread: move the accumulated events out.
    std::vector<SkillsEvent> drain();

private:
    struct Job {
        enum class Kind : unsigned char { Search, Detail, Files, Install };

        Kind kind = Kind::Search;
        std::string text;
        int limit = 30;
        agent::SkillRef ref;
        agent::SkillDetail detail;
        std::string name;
        std::string root;
        bool replace = false;
        bool enabled = true;
        agent::CancellationToken cancel;
    };

    void post(Job job, bool drop_queued_of_kind);
    void work();
    void run(const Job& job);
    void run_search(const Job& job);
    void run_detail(const Job& job);
    void run_files(const Job& job);
    void run_install_job(const Job& job);
    void emit(SkillsEvent event);
    agent::SkillInstallResult run_install(const Job& job, bool& ledger_saved);
    agent::LedgerEntry entry_for(const Job& job, const agent::SkillInstallResult& result,
                                 const std::vector<agent::SkillPayloadFile>& files) const;

    SkillsBackends backends_;
    std::thread worker_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::vector<SkillsEvent> events_;
    agent::CancellationToken search_cancel_;
    agent::CancellationToken install_cancel_;
    bool stop_ = false;
};

// Production wiring: one skills.sh adapter and one GitHub adapter captured by
// the backends. `index_opts`/`github_opts` are copied into the service.
SkillsBackends make_skills_backends(agent::SkillIndexOptions index_opts,
                                    agent::GithubOptions github_opts);

} // namespace tui

#endif // AMBER_TUI_SKILLS_SERVICE_H
