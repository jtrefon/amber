#include "watch_service.h"

#include "watch_profiles.h"

#include "agent/events.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <thread>

#include <nlohmann/json.hpp>

namespace agent::plugins {

namespace {

using json = nlohmann::json;

long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string clip(std::string s, std::size_t cap) {
    if (s.size() > cap)
        s.resize(cap);
    return s;
}

std::string brief_json(const WatchRecord& record, const WatchReport& report) {
    json evidence = json::object();
    evidence["last_status"] = record.last_status;
    evidence["observed"] = report.observed;
    evidence["artifact_path"] = report.artifact_path;
    json source = json{{"profile", record.profile},   {"label", record.label},
                       {"command", record.command},   {"pr", record.pr},
                       {"head_sha", record.head_sha}, {"attempt", record.attempt}};
    json journal = json{{"events", record.journal}, {"previous_summary", report.journal_update}};
    json brief = json{{"watch_id", record.id},
                      {"profile", record.profile},
                      {"goal", record.label},
                      {"success", "the watched work ends successfully"},
                      {"failure", "the watched work fails or times out"},
                      {"source", source},
                      {"journal", journal},
                      {"evidence", evidence}};
    std::string text = brief.dump(2);
    return clip(std::move(text), std::size_t{16} * 1024);
}

} // namespace

class WatchWorker {
public:
    WatchWorker(WatchService& service, CommandRunner& runner, WatchRecord record)
        : service_(service), runner_(runner), record_(std::move(record)) {}

    void cancel() noexcept { cancel_.request(); }

    void run() {
        ProfileHooks hooks;
        hooks.update = [this](const WatchRecord& r) { service_.update(r); };
        hooks.observe = [this](const WatchRecord& r, const WatchReport& fallback,
                               const CancellationToken& c) {
            return service_.observe(r, fallback, c);
        };
        hooks.artifact = [this](const std::string& name, const std::string& content) {
            return service_.artifact(record_.id, name, content);
        };
        WatchReport report;
        try {
            if (record_.profile == "pr")
                report = run_pr_profile(record_, runner_, hooks, cancel_);
            else if (record_.profile == "url")
                report = run_url_profile(record_, runner_, hooks, cancel_);
            else
                report = run_process_profile(record_, runner_, hooks, cancel_);
        } catch (const std::exception& e) {
            report.watch_id = record_.id;
            report.report_id = record_.id + ":exception";
            report.state = WatchState::Unknown;
            report.headline = record_.label + ": watch failed internally";
            report.observed.push_back(std::string("error: ") + e.what());
            report.needs_attention = true;
            report.attempt = record_.attempt;
        }
        service_.finish(record_, report);
    }

private:
    WatchService& service_;
    CommandRunner& runner_;
    WatchRecord record_;
    CancellationToken cancel_;
};

WatchService::WatchService(std::shared_ptr<CommandRunner> runner,
                           std::shared_ptr<ObserverModel> observer, WatchStore store,
                           std::string observer_prompt, EventBus* events, WatchSettings settings)
    : runner_(std::move(runner)), observer_(std::move(observer)), settings_(settings),
      store_(std::move(store)), observer_prompt_(std::move(observer_prompt)), events_(events) {
    for (const auto& record : store_.load_records()) {
        records_[record.id] = record;
        WatchReport loaded;
        if (store_.load_report(record.id, loaded))
            reports_[record.id] = loaded;
    }
    mark_restarted_records();
    prune();
}

WatchService::~WatchService() {
    shutdown();
}

void WatchService::mark_restarted_records() {
    std::scoped_lock lk(mtx_);
    for (auto& [id, record] : records_) {
        (void)id;
        if (watch_state_terminal(record.state))
            continue;
        record.state = WatchState::Unknown;
        record.last_status = "restarted while running";
        record.journal.emplace_back("host restarted; no execution resume");
        store_.save_record(record);
    }
}

std::string WatchService::make_id() {
    const long stamp = now_ms();
    const unsigned long n = counter_.fetch_add(1);
    return "watch_" + std::to_string(stamp) + "_" + std::to_string(n);
}

bool WatchService::validate_start(const WatchStart& options, std::string& error) {
    if (options.profile == "process" && options.command.empty()) {
        error = "watch start: process profile needs a command";
        return false;
    }
    if (options.profile == "pr" && !safe_pr_argument(options.pr)) {
        error = "watch start: pr profile needs a numeric PR or a safe PR URL";
        return false;
    }
    if (options.profile == "url" && !safe_url(options.url)) {
        error = "watch start: url profile needs an http(s) URL";
        return false;
    }
    if (options.profile != "process" && options.profile != "pr" && options.profile != "url") {
        error = "watch start: unknown profile '" + options.profile + "'";
        return false;
    }
    return true;
}

WatchRecord WatchService::make_record(const WatchStart& options) {
    WatchRecord record;
    record.id = make_id();
    record.owner_id = options.owner_id.empty() ? "default" : options.owner_id;
    record.label = options.label.empty() ? options.profile + " watch" : options.label;
    record.profile = options.profile;
    record.command = options.command;
    record.pr = options.pr;
    record.url = options.url;
    record.cwd = options.cwd;
    record.created_ms = now_ms();
    record.interval_s = options.interval_s > 0 ? options.interval_s : settings_.interval_s;
    record.deadline_s = options.deadline_s > 0 ? options.deadline_s : settings_.deadline_s;
    record.success_exit = options.success_exit;
    record.observer = options.observer && observer_ && !observer_prompt_.empty();
    record.artifact_dir = store_.artifact_dir(record.id);
    record.journal.emplace_back("queued");
    return record;
}

void WatchService::launch(const WatchRecord& record) {
    auto worker = std::make_shared<WatchWorker>(*this, *runner_, record);
    {
        std::scoped_lock lk(mtx_);
        records_[record.id] = record;
        workers_[record.id] = worker;
    }
    // Start outside the lock: the worker's first update() takes it.
    std::thread thread([worker] { worker->run(); });
    {
        std::scoped_lock lk(mtx_);
        threads_[record.id] = std::move(thread);
    }
}

std::string WatchService::start(const WatchStart& options, std::string& error) {
    error.clear();
    if (!validate_start(options, error))
        return {};
    if (counts().running >= settings_.max_running) {
        error = "watch start: too many running watches (limit " +
                std::to_string(settings_.max_running) + ")";
        return {};
    }
    const WatchRecord record = make_record(options);
    if (!store_.save_record(record)) {
        error = "watch start: could not persist the watch record";
        return {};
    }
    launch(record);
    return record.id;
}

std::vector<WatchRecord> WatchService::list() const {
    std::scoped_lock lk(mtx_);
    std::vector<WatchRecord> out;
    out.reserve(records_.size());
    for (const auto& [id, record] : records_) {
        (void)id;
        out.push_back(record);
    }
    return out;
}

std::optional<WatchRecord> WatchService::get(const std::string& id) const {
    std::scoped_lock lk(mtx_);
    auto it = records_.find(id);
    if (it == records_.end())
        return std::nullopt;
    return it->second;
}

std::optional<WatchReport> WatchService::report(const std::string& id) const {
    {
        std::scoped_lock lk(mtx_);
        auto it = reports_.find(id);
        if (it != reports_.end())
            return it->second;
    }
    WatchReport report;
    if (store_.load_report(id, report))
        return report;
    return std::nullopt;
}

bool WatchService::cancel(const std::string& id) {
    std::shared_ptr<WatchWorker> worker;
    {
        std::scoped_lock lk(mtx_);
        auto it = workers_.find(id);
        if (it == workers_.end())
            return false;
        worker = it->second;
    }
    worker->cancel();
    return true;
}

void WatchService::cancel_owner(const std::string& owner_id) {
    std::vector<std::shared_ptr<WatchWorker>> workers;
    {
        std::scoped_lock lk(mtx_);
        for (const auto& [id, record] : records_) {
            if (record.owner_id != owner_id || watch_state_terminal(record.state))
                continue;
            auto it = workers_.find(id);
            if (it != workers_.end())
                workers.push_back(it->second);
        }
    }
    for (auto& worker : workers)
        worker->cancel();
}

std::optional<WatchRecord> WatchService::wait(const std::string& id, int timeout_s) const {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto record = get(id); record && watch_state_terminal(record->state))
            return record;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return get(id);
}

WatchCounts WatchService::counts() const {
    WatchCounts out;
    std::scoped_lock lk(mtx_);
    for (const auto& [id, record] : records_) {
        if (!watch_state_terminal(record.state))
            ++out.running;
        auto it = reports_.find(id);
        if (it != reports_.end() && it->second.needs_attention)
            ++out.attention;
    }
    out.observers = observers_.load();
    return out;
}

void WatchService::shutdown() {
    if (shutting_down_.exchange(true))
        return;
    std::vector<std::shared_ptr<WatchWorker>> workers;
    {
        std::scoped_lock lk(mtx_);
        for (auto& [id, worker] : workers_)
            workers.push_back(worker);
    }
    for (auto& worker : workers)
        worker->cancel();
    std::map<std::string, std::thread> threads;
    {
        std::scoped_lock lk(mtx_);
        threads.swap(threads_);
    }
    for (auto& [id, thread] : threads) {
        (void)id;
        if (thread.joinable())
            thread.join();
    }
    std::scoped_lock lk(mtx_);
    workers_.clear();
}

void WatchService::update(const WatchRecord& record) {
    {
        std::scoped_lock lk(mtx_);
        records_[record.id] = record;
    }
    store_.save_record(record);
}

void WatchService::finish(const WatchRecord& record, const WatchReport& report) {
    WatchRecord final = record;
    final.state = report.state;
    final.ended_ms = now_ms();
    final.report_id = report.report_id;
    final.last_status = report.headline;
    final.attempt = report.attempt;
    if (!report.journal_update.empty())
        final.journal.push_back(report.journal_update);
    {
        std::scoped_lock lk(mtx_);
        auto existing = records_.find(final.id);
        if (existing != records_.end()) {
            final.abandoned = final.abandoned || existing->second.abandoned;
            final.delivered = final.delivered || existing->second.delivered;
        }
        final.delivered = final.delivered || final.abandoned;
        records_[final.id] = final;
        reports_[final.id] = report;
    }
    store_.save_record(final);
    store_.save_report(report);
    prune();
    if (!final.abandoned)
        publish_report(final, report);
}

void WatchService::ack(const std::string& watch_id) {
    WatchRecord record;
    {
        std::scoped_lock lk(mtx_);
        auto it = records_.find(watch_id);
        if (it == records_.end())
            return;
        it->second.delivered = true;
        record = it->second;
    }
    store_.save_record(record);
}

void WatchService::abandon_owner(const std::string& owner_id) {
    std::vector<WatchRecord> changed;
    std::vector<std::shared_ptr<WatchWorker>> workers;
    {
        std::scoped_lock lk(mtx_);
        for (auto& [id, record] : records_) {
            if (record.owner_id != owner_id)
                continue;
            record.abandoned = true;
            if (watch_state_terminal(record.state))
                record.delivered = true;
            changed.push_back(record);
            auto it = workers_.find(id);
            if (it != workers_.end())
                workers.push_back(it->second);
        }
    }
    for (const auto& record : changed)
        store_.save_record(record);
    for (auto& worker : workers)
        worker->cancel();
}

std::vector<WatchReport> WatchService::pending_reports() const {
    std::vector<WatchReport> out;
    std::scoped_lock lk(mtx_);
    for (const auto& [id, record] : records_) {
        if (!watch_state_terminal(record.state) || record.delivered || record.abandoned)
            continue;
        auto it = reports_.find(id);
        if (it != reports_.end())
            out.push_back(it->second);
    }
    return out;
}

void WatchService::reemit_pending() {
    if (!events_ || !events_->has_subscribers(EventType::WatchReport))
        return;
    std::vector<std::pair<WatchRecord, WatchReport>> pending;
    {
        std::scoped_lock lk(mtx_);
        for (const auto& [id, record] : records_) {
            if (!watch_state_terminal(record.state) || record.delivered || record.abandoned)
                continue;
            auto it = reports_.find(id);
            if (it != reports_.end())
                pending.emplace_back(record, it->second);
        }
    }
    for (const auto& item : pending)
        publish_report(item.first, item.second);
}

int WatchService::running_count() const {
    return counts().running;
}

void WatchService::prune() {
    const long now = now_ms();
    const long max_age_ms = static_cast<long>(settings_.retention_days) * 24 * 60 * 60 * 1000;
    std::vector<WatchRecord> terminal;
    {
        std::scoped_lock lk(mtx_);
        for (const auto& [id, record] : records_) {
            if (watch_state_terminal(record.state))
                terminal.push_back(record);
        }
    }
    std::sort(terminal.begin(), terminal.end(),
              [](const WatchRecord& a, const WatchRecord& b) { return a.ended_ms < b.ended_ms; });

    std::vector<std::string> remove;
    const std::size_t keep =
        settings_.retention > 0 ? static_cast<std::size_t>(settings_.retention) : 0;
    for (std::size_t i = 0; i < terminal.size(); ++i) {
        const auto& record = terminal[i];
        const bool too_old =
            max_age_ms > 0 && record.ended_ms > 0 && now - record.ended_ms > max_age_ms;
        const bool over_cap = terminal.size() - i > keep;
        if (too_old || over_cap)
            remove.push_back(record.id);
    }
    if (remove.empty())
        return;
    {
        std::scoped_lock lk(mtx_);
        for (const auto& id : remove) {
            records_.erase(id);
            reports_.erase(id);
        }
    }
    for (const auto& id : remove)
        store_.remove(id);
}

void WatchService::publish_report(const WatchRecord& record, const WatchReport& report) {
    if (!events_)
        return;
    WatchReportEvent event;
    event.owner_id = record.owner_id;
    event.watch_id = record.id;
    event.report_id = report.report_id;
    event.state = watch_state_name(report.state);
    event.headline = report.headline;
    event.needs_attention = report.needs_attention;
    Events(*events_).publish(event);
}

WatchReport WatchService::observe(const WatchRecord& record, const WatchReport& fallback,
                                  const CancellationToken& cancel) {
    if (!observer_ || !record.observer || observer_prompt_.empty() || cancel.is_requested())
        return fallback;
    std::scoped_lock lk(observer_mtx_);
    // cppcheck-suppress knownConditionTrueFalse -- cancellation can race the lock.
    if (cancel.is_requested())
        return fallback;
    ++observers_;
    std::string raw;
    try {
        raw = observer_->complete(observer_prompt_, brief_json(record, fallback), cancel);
    } catch (const std::exception&) {
        raw.clear();
    }
    --observers_;
    if (raw.empty()) {
        WatchReport out = fallback;
        out.observed.emplace_back("observer unavailable; deterministic report kept");
        return out;
    }
    return parse_observer_report(raw, fallback);
}

std::string WatchService::artifact(const std::string& watch_id, const std::string& name,
                                   const std::string& content) {
    return store_.write_artifact(watch_id, name, content);
}

} // namespace agent::plugins
