#include "agent/task_inbox.h"

#include "agent/task_format.h"

#include <algorithm>
#include <utility>

namespace agent {

namespace {

constexpr std::size_t kLineCap = 512;
constexpr std::size_t kDigestCap = std::size_t{8} * 1024;
constexpr std::size_t kAckedWindow = 4096;

std::string clip(std::string text, std::size_t cap) {
    if (text.size() > cap)
        text.resize(cap);
    return text;
}

std::string one_line(const TaskRecord& record) {
    std::string line = record.id + " (" + task_kind_name(record.kind);
    if (!record.label.empty())
        line += ", \"" + clip(record.label, 64) + "\"";
    line += ") ";
    line += task_state_name(record.state);
    const std::string took = format_task_duration(record.started_ms, record.ended_ms);
    if (!took.empty())
        line += " in " + took;
    if (record.state == TaskState::Succeeded || record.state == TaskState::Failed)
        line += ", exit " + std::to_string(record.result.exit_code);
    if (!record.result.summary.empty())
        line += "; " + clip(record.result.summary, 160);
    if (!record.error.empty())
        line += "; " + clip(record.error, 160);
    return clip(std::move(line), kLineCap);
}

} // namespace

std::string format_task_digest(const std::vector<TaskCompletion>& completions) {
    if (completions.empty())
        return {};
    std::string out = "[harness] background task report (automated event, not a user message)\n";
    out += std::to_string(completions.size()) + " completed:\n";
    for (const auto& completion : completions) {
        std::string line = "- " + completion.prompt_text + "\n";
        if (!completion.artifact_path.empty())
            line += "  artifact: " + completion.artifact_path + "\n";
        if (out.size() + line.size() > kDigestCap) {
            out += "- (more completions are available in /tasks)\n";
            break;
        }
        out += line;
    }
    return out;
}

TaskInbox::TaskInbox(EventBus& events, TaskService& tasks)
    : tasks_(tasks), subscription_(Events(events).subscribe<TaskCompletedEvent>(
                         [this](const TaskCompletedEvent& event) { on_completed(event); })) {}

TaskInbox::~TaskInbox() {
    subscription_.release();
}

void TaskInbox::on_completed(const TaskCompletedEvent& event) {
    auto record = tasks_.get(event.id);
    if (!record || record->notify == TaskNotify::Silent)
        return;
    TaskCompletion completion;
    completion.task_id = record->id;
    completion.backend_id = tasks_.backend_id(record->id);
    completion.owner_id = record->owner.owner_id;
    completion.kind = record->kind;
    completion.state = record->state;
    completion.prompt_text = one_line(*record);
    completion.artifact_path = record->result.artifact_path;
    completion.created_ms = record->created_ms;
    post(std::move(completion));
}

void TaskInbox::post(TaskCompletion completion) {
    std::scoped_lock lk(mtx_);
    if (acked_.count(completion.task_id) > 0)
        return;
    pending_[completion.owner_id][completion.task_id] = std::move(completion);
}

std::vector<TaskCompletion> TaskInbox::take(const std::string& owner_id) {
    std::scoped_lock lk(mtx_);
    auto owner = pending_.find(owner_id);
    if (owner == pending_.end())
        return {};
    std::vector<TaskCompletion> out;
    for (const auto& [id, completion] : owner->second) {
        if (taken_.count(id) == 0)
            out.push_back(completion);
    }
    std::sort(out.begin(), out.end(), [](const TaskCompletion& a, const TaskCompletion& b) {
        if (a.created_ms != b.created_ms)
            return a.created_ms < b.created_ms;
        return a.task_id < b.task_id;
    });
    for (const auto& completion : out)
        taken_.insert(completion.task_id);
    return out;
}

void TaskInbox::set_ack_hook(AckHook hook) {
    std::scoped_lock lk(mtx_);
    ack_hook_ = std::move(hook);
}

void TaskInbox::ack(const std::string& owner_id, const std::vector<std::string>& ids) {
    std::vector<TaskCompletion> acked;
    AckHook hook;
    {
        std::scoped_lock lk(mtx_);
        hook = ack_hook_;
        auto owner = pending_.find(owner_id);
        if (owner != pending_.end()) {
            for (const auto& id : ids) {
                auto found = owner->second.find(id);
                if (found == owner->second.end())
                    continue;
                acked.push_back(found->second);
                owner->second.erase(found);
                taken_.erase(id);
                acked_.insert(id);
            }
            if (owner->second.empty())
                pending_.erase(owner);
        }
        if (acked_.size() > kAckedWindow)
            acked_.clear(); // the idempotency window is bounded, not eternal
    }
    if (hook) {
        for (const auto& completion : acked)
            hook(completion);
    }
}

void TaskInbox::drop(const std::string& owner_id) {
    std::scoped_lock lk(mtx_);
    auto owner = pending_.find(owner_id);
    if (owner == pending_.end())
        return;
    for (const auto& [id, completion] : owner->second) {
        (void)completion;
        taken_.erase(id);
    }
    pending_.erase(owner);
}

std::size_t TaskInbox::pending(const std::string& owner_id) const {
    std::scoped_lock lk(mtx_);
    auto owner = pending_.find(owner_id);
    return owner == pending_.end() ? 0u : owner->second.size();
}

std::size_t TaskInbox::size() const {
    std::scoped_lock lk(mtx_);
    std::size_t total = 0;
    for (const auto& [owner, items] : pending_) {
        (void)owner;
        total += items.size();
    }
    return total;
}

} // namespace agent
