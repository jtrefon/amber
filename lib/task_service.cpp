#include "agent/task.h"

#include "agent/event_bus.h"
#include "agent/events.h"
#include "agent/run_scope.h"
#include "agent/workspace.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <random>

namespace agent {

namespace {

constexpr std::size_t kLabelCap = 128;
constexpr std::size_t kProgressCap = 512;
constexpr std::size_t kSummaryCap = std::size_t{2} * 1024;
constexpr std::size_t kOutputTailCap = std::size_t{8} * 1024;
constexpr std::size_t kErrorCap = 1024;

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string clip(std::string text, std::size_t cap) {
    if (text.size() > cap)
        text.resize(cap);
    return text;
}

std::string base32(std::uint64_t value) {
    static constexpr char kAlphabet[] = "0123456789abcdefghijklmnopqrstuv";
    std::string out;
    do {
        out.push_back(kAlphabet[value & 31u]);
        value >>= 5u;
    } while (value != 0);
    std::reverse(out.begin(), out.end());
    return out;
}

std::string make_id() {
    static std::atomic<std::uint64_t> counter{0};
    static thread_local std::mt19937_64 rng{
        std::random_device{}() ^
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())};
    const std::uint64_t mixed = rng() ^ ((++counter) * 0x9e3779b97f4a7c15ULL);
    return "task_" + base32(static_cast<std::uint64_t>(now_ms())) + "_" + base32(mixed);
}

std::string limit_error(TaskKind kind) {
    return std::string("task limit reached (") + task_kind_name(kind) + "); do not retry";
}

void normalize(TaskRecord& record) {
    if (record.id.empty())
        record.id = make_id();
    if (record.created_ms == 0)
        record.created_ms = now_ms();
    record.state = TaskState::Queued;
    record.label = clip(std::move(record.label), kLabelCap);
    record.progress = clip(std::move(record.progress), kProgressCap);
    record.error = clip(std::move(record.error), kErrorCap);
    record.result.summary = clip(std::move(record.result.summary), kSummaryCap);
    record.result.output_tail = clip(std::move(record.result.output_tail), kOutputTailCap);
}

} // namespace

bool task_state_terminal(TaskState state) noexcept {
    switch (state) {
    case TaskState::Queued:
    case TaskState::Running:
        return false;
    default:
        return true;
    }
}

const char* task_state_name(TaskState state) noexcept {
    switch (state) {
    case TaskState::Queued:
        return "queued";
    case TaskState::Running:
        return "running";
    case TaskState::Succeeded:
        return "succeeded";
    case TaskState::Failed:
        return "failed";
    case TaskState::Cancelled:
        return "cancelled";
    case TaskState::TimedOut:
        return "timed_out";
    case TaskState::Interrupted:
        return "interrupted";
    case TaskState::Unknown:
        return "unknown";
    }
    return "unknown";
}

TaskOwner task_owner_from_scope() {
    TaskOwner owner;
    owner.owner_id = run_owner_id();
    if (const RunScopeChain* scope = current_run_scope(); scope && scope->leaf.project_root)
        owner.project_root = *scope->leaf.project_root;
    else
        owner.project_root = Workspace::workspace_root();
    if (const std::string* root = scoped_workspace_root())
        owner.worktree = *root;
    return owner;
}

const char* task_kind_name(TaskKind kind) noexcept {
    switch (kind) {
    case TaskKind::Command:
        return "command";
    case TaskKind::Watch:
        return "watch";
    case TaskKind::Worker:
        return "worker";
    }
    return "command";
}

TaskService::TaskService(EventBus& events, TaskLimits limits) : events_(&events), limits_(limits) {}

bool TaskService::legal_transition(TaskState from, TaskState to) const noexcept {
    switch (from) {
    case TaskState::Queued:
        return to == TaskState::Running || to == TaskState::Cancelled || to == TaskState::Unknown;
    case TaskState::Running:
        return to == TaskState::Succeeded || to == TaskState::Failed ||
               to == TaskState::Cancelled || to == TaskState::TimedOut ||
               to == TaskState::Interrupted || to == TaskState::Unknown;
    default:
        return false;
    }
}

TaskService::EventKind TaskService::transition_locked(Entry& entry, TaskState to,
                                                      EventKind event) noexcept {
    if (!legal_transition(entry.record.state, to))
        return EventKind::None;
    entry.record.state = to;
    ++generation_;
    return event;
}

std::string TaskService::create(TaskRecord record, std::string* error) {
    normalize(record);
    std::string id = record.id;
    {
        std::scoped_lock lk(mtx_);
        if (over_limit_locked(record)) {
            if (error)
                *error = limit_error(record.kind);
            return {};
        }
        tasks_[id] = Entry{std::move(record), {}};
        ++generation_;
    }
    return id;
}

std::pair<int, int> TaskService::caps_for(TaskKind kind) const noexcept {
    switch (kind) {
    case TaskKind::Command:
        return {limits_.command_per_owner, limits_.command_global};
    case TaskKind::Watch:
        return {limits_.watch_per_owner, limits_.watch_global};
    case TaskKind::Worker:
        return {limits_.worker_per_owner, limits_.worker_global};
    }
    return {0, 0};
}

bool TaskService::over_limit_locked(const TaskRecord& record) const noexcept {
    int owner_kind = 0;
    int global_kind = 0;
    int owner_total = 0;
    int global_total = 0;
    for (const auto& [id, entry] : tasks_) {
        (void)id;
        if (task_state_terminal(entry.record.state))
            continue;
        ++global_total;
        if (entry.record.owner.owner_id == record.owner.owner_id)
            ++owner_total;
        if (entry.record.kind == record.kind) {
            ++global_kind;
            if (entry.record.owner.owner_id == record.owner.owner_id)
                ++owner_kind;
        }
    }
    const auto [per_owner, per_global] = caps_for(record.kind);
    return (per_owner > 0 && owner_kind >= per_owner) ||
           (per_global > 0 && global_kind >= per_global) ||
           (limits_.total_per_owner > 0 && owner_total >= limits_.total_per_owner) ||
           (limits_.total_global > 0 && global_total >= limits_.total_global);
}

bool TaskService::started(const std::string& id, std::int64_t started_at) {
    TaskStartedEvent event;
    {
        std::scoped_lock lk(mtx_);
        auto it = tasks_.find(id);
        if (it == tasks_.end())
            return false;
        if (transition_locked(it->second, TaskState::Running, EventKind::Started) ==
            EventKind::None)
            return false;
        it->second.record.started_ms = started_at == 0 ? now_ms() : started_at;
        event.id = id;
        event.kind = it->second.record.kind;
        event.owner_id = it->second.record.owner.owner_id;
        event.label = it->second.record.label;
    }
    if (events_)
        Events(*events_).publish(event);
    return true;
}

bool TaskService::progress(const std::string& id, std::string line) {
    TaskProgressEvent event;
    {
        std::scoped_lock lk(mtx_);
        auto it = tasks_.find(id);
        if (it == tasks_.end() || task_state_terminal(it->second.record.state))
            return false;
        it->second.record.progress = clip(std::move(line), kProgressCap);
        ++generation_;
        event.id = id;
        event.line = it->second.record.progress;
    }
    if (events_)
        Events(*events_).publish(event);
    return true;
}

bool TaskService::finish(const std::string& id, TaskState state, TaskResult result,
                         std::string error) {
    if (!task_state_terminal(state) || state == TaskState::Cancelled)
        return false;
    TaskCompletedEvent event;
    {
        std::scoped_lock lk(mtx_);
        auto it = tasks_.find(id);
        if (it == tasks_.end() || cancelling_.count(id) > 0)
            return false;
        if (transition_locked(it->second, state, EventKind::Completed) == EventKind::None)
            return false;
        it->second.record.ended_ms = now_ms();
        it->second.record.result.exit_code = result.exit_code;
        it->second.record.result.signal = result.signal;
        it->second.record.result.summary = clip(std::move(result.summary), kSummaryCap);
        it->second.record.result.output_tail = clip(std::move(result.output_tail), kOutputTailCap);
        it->second.record.result.artifact_path = std::move(result.artifact_path);
        it->second.record.error = clip(std::move(error), kErrorCap);
        event.id = id;
        event.state = state;
        event.exit_code = it->second.record.result.exit_code;
        event.summary = it->second.record.result.summary;
        event.error = it->second.record.error;
    }
    cv_.notify_all();
    if (events_)
        Events(*events_).publish(event);
    return true;
}

bool TaskService::cancel(const std::string& id, std::string reason,
                         const CancellationToken& token) {
    std::function<bool(const CancellationToken&)> handler;
    {
        std::scoped_lock lk(mtx_);
        auto it = tasks_.find(id);
        if (it == tasks_.end() || task_state_terminal(it->second.record.state))
            return false;
        cancelling_.insert(id);
        handler = it->second.cancel;
    }

    bool stopped = false;
    if (handler) {
        try {
            stopped = handler(token);
        } catch (...) {
            stopped = false;
        }
    }
    return finish_cancel(id, stopped, clip(std::move(reason), kErrorCap));
}

bool TaskService::finish_cancel(const std::string& id, bool stopped, const std::string& reason) {
    TaskCancelledEvent cancelled;
    TaskCompletedEvent unknown;
    {
        std::scoped_lock lk(mtx_);
        auto it = tasks_.find(id);
        if (it == tasks_.end() || task_state_terminal(it->second.record.state)) {
            cancelling_.erase(id);
            return false;
        }
        const TaskState to = stopped ? TaskState::Cancelled : TaskState::Unknown;
        const bool moved =
            transition_locked(it->second, to, EventKind::Cancelled) != EventKind::None;
        cancelling_.erase(id);
        if (!moved)
            return false;
        it->second.record.ended_ms = now_ms();
        it->second.record.cancel_reason = reason;
        if (stopped) {
            cancelled.id = id;
            cancelled.reason = reason;
        } else {
            it->second.record.error = reason;
            unknown.id = id;
            unknown.state = TaskState::Unknown;
            unknown.error = reason;
        }
    }
    cv_.notify_all();
    if (events_) {
        if (stopped)
            Events(*events_).publish(cancelled);
        else
            Events(*events_).publish(unknown);
    }
    return true;
}

bool TaskService::attach_cancel(const std::string& id,
                                std::function<bool(const CancellationToken&)> fn) {
    std::scoped_lock lk(mtx_);
    auto it = tasks_.find(id);
    if (it == tasks_.end() || task_state_terminal(it->second.record.state))
        return false;
    it->second.cancel = std::move(fn);
    return true;
}

std::string TaskService::adopt_job(const std::string& job_id, TaskRecord record,
                                   std::string* error) {
    std::string id = create(std::move(record), error);
    if (id.empty())
        return {};
    std::scoped_lock lk(mtx_);
    backends_[id] = job_id;
    return id;
}

bool TaskService::bind_backend(const std::string& id, const std::string& backend_id) {
    std::scoped_lock lk(mtx_);
    auto it = tasks_.find(id);
    if (it == tasks_.end() || task_state_terminal(it->second.record.state))
        return false;
    backends_[id] = backend_id;
    return true;
}

std::string TaskService::backend_id(const std::string& id) const {
    std::scoped_lock lk(mtx_);
    auto it = backends_.find(id);
    return it == backends_.end() ? std::string() : it->second;
}

std::optional<TaskRecord> TaskService::wait_for_terminal(const std::string& id, int timeout_ms,
                                                         const CancellationToken& token) const {
    std::unique_lock lk(mtx_);
    const auto ready = [&] {
        auto it = tasks_.find(id);
        return it != tasks_.end() && task_state_terminal(it->second.record.state);
    };
    cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                 [&] { return ready() || token.is_requested(); });
    if (token.is_requested() || !ready()) {
        return std::nullopt;
    }
    auto it = tasks_.find(id);
    if (it == tasks_.end())
        return std::nullopt;
    return it->second.record;
}

std::optional<TaskRecord> TaskService::get(const std::string& id) const {
    std::scoped_lock lk(mtx_);
    auto it = tasks_.find(id);
    if (it == tasks_.end())
        return std::nullopt;
    return it->second.record;
}

std::vector<TaskRecord> TaskService::list(const std::string& owner_id) const {
    std::scoped_lock lk(mtx_);
    std::vector<TaskRecord> out;
    for (const auto& [id, entry] : tasks_) {
        (void)id;
        if (entry.record.owner.owner_id == owner_id)
            out.push_back(entry.record);
    }
    return out;
}

std::vector<TaskRecord> TaskService::all() const {
    std::scoped_lock lk(mtx_);
    std::vector<TaskRecord> out;
    out.reserve(tasks_.size());
    for (const auto& [id, entry] : tasks_) {
        (void)id;
        out.push_back(entry.record);
    }
    return out;
}

std::uint64_t TaskService::generation() const {
    std::scoped_lock lk(mtx_);
    return generation_;
}

TaskLimits TaskService::limits() const {
    std::scoped_lock lk(mtx_);
    return limits_;
}

} // namespace agent
