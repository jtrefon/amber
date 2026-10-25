#include "tui/skills_service.h"

#include "agent/skill_ledger.h"

#include <algorithm>
#include <chrono>
#include <memory>

namespace tui {

namespace {

long long wall_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

SkillsService::SkillsService(SkillsBackends backends) : backends_(std::move(backends)) {
    worker_ = std::thread([this] { work(); });
}

SkillsService::~SkillsService() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    search_cancel_.request();
    install_cancel_.request();
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();
}

void SkillsService::post(Job job, bool drop_queued_of_kind) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (drop_queued_of_kind) {
            jobs_.erase(
                std::remove_if(jobs_.begin(), jobs_.end(),
                               [&job](const Job& queued) { return queued.kind == job.kind; }),
                jobs_.end());
        }
        jobs_.push_back(std::move(job));
    }
    cv_.notify_one();
}

void SkillsService::search(std::string query, int limit) {
    search_cancel_.request();
    search_cancel_ = agent::CancellationToken{};

    Job job;
    job.kind = Job::Kind::Search;
    job.text = std::move(query);
    job.limit = limit;
    job.cancel = search_cancel_;
    post(std::move(job), true);
}

void SkillsService::detail(agent::SkillRef ref) {
    Job job;
    job.kind = Job::Kind::Detail;
    job.ref = std::move(ref);
    post(std::move(job), true);
}

void SkillsService::install(agent::SkillDetail detail, std::string name, std::string root,
                            bool replace, bool enabled) {
    install_cancel_.request();
    install_cancel_ = agent::CancellationToken{};

    Job job;
    job.kind = Job::Kind::Install;
    job.detail = std::move(detail);
    job.name = std::move(name);
    job.root = std::move(root);
    job.replace = replace;
    job.enabled = enabled;
    job.cancel = install_cancel_;
    post(std::move(job), true);
}

void SkillsService::cancel() {
    search_cancel_.request();
    install_cancel_.request();
}

std::vector<SkillsEvent> SkillsService::drain() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<SkillsEvent> out;
    out.swap(events_);
    return out;
}

void SkillsService::emit(SkillsEvent event) {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back(std::move(event));
}

void SkillsService::work() {
    while (true) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
            if (stop_ && jobs_.empty())
                return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        run(job);
    }
}

void SkillsService::run(const Job& job) {
    switch (job.kind) {
    case Job::Kind::Search: {
        SkillsEvent event;
        event.kind = SkillsEvent::Kind::SearchDone;
        event.query = job.text;
        if (backends_.search) {
            event.search = backends_.search(job.text, job.limit, job.cancel);
        } else {
            event.search.error = {agent::IndexError::Unsupported, 0, 0, "no search backend"};
        }
        emit(std::move(event));
        return;
    }
    case Job::Kind::Detail: {
        SkillsEvent event;
        event.kind = SkillsEvent::Kind::DetailDone;
        if (backends_.detail) {
            event.detail = backends_.detail(job.ref, job.cancel);
        } else {
            event.detail.error = {agent::IndexError::Unsupported, 0, 0, "no detail backend"};
        }
        emit(std::move(event));
        return;
    }
    case Job::Kind::Install: {
        bool ledger_saved = false;
        SkillsEvent event;
        event.kind = SkillsEvent::Kind::InstallDone;
        event.name = job.name;
        event.install = run_install(job, ledger_saved);
        event.ledger_saved = ledger_saved;
        emit(std::move(event));
        return;
    }
    }
}

agent::SkillInstallResult SkillsService::run_install(const Job& job, bool& ledger_saved) {
    agent::SkillInstallResult result;
    if (!backends_.assets) {
        result.error = "no asset backend";
        return result;
    }
    const agent::AssetsResult assets = backends_.assets(job.detail, job.cancel);
    if (!assets.files) {
        result.error =
            assets.error.detail.empty() ? "cannot fetch skill files" : assets.error.detail;
        return result;
    }

    agent::SkillInstallRequest request;
    request.name = job.name;
    request.dest_root = job.root;
    request.replace = job.replace;
    request.files.reserve(assets.files->size());
    for (const auto& file : *assets.files)
        request.files.push_back({file.path, file.contents});

    result = backends_.install ? backends_.install(request) : agent::install_skill_files(request);
    if (!result.ok)
        return result;

    agent::SkillLedger ledger = agent::SkillLedger::for_root(job.root);
    ledger.load();
    agent::LedgerEntry entry;
    entry.id = job.detail.ref.id;
    entry.source = job.detail.ref.source;
    entry.slug = agent::skill_slug(job.detail.ref.id);
    entry.ref = job.detail.default_branch;
    entry.hash = job.detail.hash;
    entry.category = job.detail.category;
    entry.installed_at_ms = wall_ms();
    entry.files = result.files;
    entry.bytes = result.bytes;
    entry.enabled = job.enabled;
    ledger_saved = ledger.upsert(job.name, std::move(entry));
    return result;
}

SkillsBackends make_skills_backends(agent::SkillIndexOptions index_opts,
                                    agent::GithubOptions github_opts) {
    auto index = std::make_shared<agent::SkillsShIndex>(std::move(index_opts));
    auto github = std::make_shared<agent::GitHubIndex>(std::move(github_opts));

    SkillsBackends backends;
    backends.search = [index](const std::string& query, int limit,
                              const agent::CancellationToken& cancel) {
        return index->search(query, limit, cancel);
    };
    backends.detail = [github](const agent::SkillRef& ref, const agent::CancellationToken& cancel) {
        return github->detail(ref, cancel);
    };
    backends.assets = [github](const agent::SkillDetail& detail,
                               const agent::CancellationToken& cancel) {
        return github->fetch_assets(detail, cancel);
    };
    backends.install = [](const agent::SkillInstallRequest& request) {
        return agent::install_skill_files(request);
    };
    return backends;
}

} // namespace tui
