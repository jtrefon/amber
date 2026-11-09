#include "agent/subagent.h"

#include "agent/event_bus.h"
#include "agent/private_file.h"
#include "agent/run_scope.h"
#include "agent/task_store.h"

#include <algorithm>
#include <chrono>
#include <filesystem>

namespace agent {

namespace {

thread_local bool t_in_subagent = false;
thread_local bool t_subagent_inherited = false;

constexpr std::size_t kFieldCap = std::size_t{16} * 1024;
constexpr std::size_t kLabelCap = 128;
constexpr std::size_t kReportCap = std::size_t{8} * 1024;
constexpr int kCancelWaitMs = 5000;

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

std::string head_of(const std::string& text, std::size_t cap) {
    if (text.size() <= cap)
        return text;
    return text.substr(0, cap > 3 ? cap - 3 : cap) + "...";
}

const char* approval_result(Approval approval) {
    const bool allowed = approval == Approval::AllowOnce || approval == Approval::AllowSession ||
                         approval == Approval::AlwaysAllow;
    return allowed ? "allowed" : "denied";
}

std::string compose_prompt(const SpawnRequest& request) {
    return "## Task\n\n" + request.goal + "\n\n## Context\n\n" + request.context +
           "\n\n## Report\n\n" + request.expect;
}

std::string message_line(const Message& message) {
    json j;
    j["role"] = message.role;
    j["content"] = message.content;
    if (!message.reasoning.empty())
        j["reasoning"] = message.reasoning;
    if (!message.tool_call_id.empty())
        j["tool_call_id"] = message.tool_call_id;
    if (!message.name.empty())
        j["name"] = message.name;
    if (!message.tool_calls.is_null())
        j["tool_calls"] = message.tool_calls;
    if (message.harness)
        j["harness"] = true;
    return j.dump();
}

std::string serialize_context(const Context& context) {
    std::string out;
    for (const auto& message : context.get_all()) {
        out += message_line(message);
        out += "\n";
    }
    return out;
}

bool validate_request(const SpawnRequest& request, std::string& err) {
    if (request.goal.empty()) {
        err = "delegate: goal is required";
        return false;
    }
    if (request.context.empty()) {
        err = "delegate: context is required (the worker does not see the parent conversation)";
        return false;
    }
    if (request.expect.empty()) {
        err = "delegate: expect is required (state the report shape and success criteria)";
        return false;
    }
    if (request.goal.size() > kFieldCap || request.context.size() > kFieldCap ||
        request.expect.size() > kFieldCap) {
        err = "delegate: goal, context and expect are each limited to 16 KiB";
        return false;
    }
    if (request.max_turns < 0 || request.max_turns > 1000) {
        err = "delegate: max_turns must be between 0 and 1000";
        return false;
    }
    return true;
}

} // namespace

std::string worker_directive() {
    return "## Worker directive\n\n"
           "You are a focused worker agent executing one task on behalf of "
           "the main agent. You do not see the main conversation; the task "
           "below carries everything you need. Work autonomously with the "
           "available tools. When the task is complete, reply with the report "
           "the task asks for: what you did, what you found, and what remains "
           "if anything.";
}

bool in_subagent() noexcept {
    return t_in_subagent || t_subagent_inherited;
}

void set_subagent_inherited(bool value) noexcept {
    t_subagent_inherited = value;
}

void append_task_tail(std::string& tail, const std::string& line) {
    if (!tail.empty() && tail.back() != '\n')
        tail += '\n';
    tail += line + '\n';
    std::size_t lines = static_cast<std::size_t>(std::count(tail.begin(), tail.end(), '\n'));
    while (lines > kTaskTailMaxLines) {
        const std::size_t end = tail.find('\n');
        if (end == std::string::npos)
            break;
        tail.erase(0, end + 1);
        --lines;
    }
    while (tail.size() > kTaskTailMaxBytes) {
        const std::size_t end = tail.find('\n');
        if (end == std::string::npos) {
            tail.erase(0, tail.size() - kTaskTailMaxBytes);
            break;
        }
        tail.erase(0, end + 1);
    }
}

SubAgentExecutor::SubAgentExecutor() = default;

SubAgentExecutor::~SubAgentExecutor() {
    shutdown();
}

void SubAgentExecutor::set_tasks(TaskService* tasks) noexcept {
    std::scoped_lock lk(queue_mutex_);
    tasks_ = tasks;
    internal_tasks_.reset();
    internal_bus_.reset();
}

void SubAgentExecutor::ensure_workers() {
    std::scoped_lock lk(queue_mutex_);
    const int target = parallel_.load() ? max_.load() : 1;
    while (worker_count_ < target) {
        workers_.emplace_back([this] { worker_loop(); });
        ++worker_count_;
    }
}

int SubAgentExecutor::active() const noexcept {
    std::scoped_lock lk(queue_mutex_);
    return static_cast<int>(running_.size());
}

int SubAgentExecutor::waiting() const noexcept {
    std::scoped_lock lk(queue_mutex_);
    return static_cast<int>(queue_.size());
}

std::vector<TaskRecord> SubAgentExecutor::tasks() const {
    if (!tasks_)
        return {};
    std::vector<TaskRecord> out;
    for (auto& record : tasks_->all()) {
        if (record.kind == TaskKind::Worker)
            out.push_back(std::move(record));
    }
    return out;
}

std::string SubAgentExecutor::transcript_path(const std::string& task_id) const {
    if (artifact_root_.empty() || task_id.empty())
        return {};
    return artifact_root_ + "/" + task_id + "/transcript.jsonl";
}

void SubAgentExecutor::ensure_tasks() {
    std::scoped_lock lk(queue_mutex_);
    if (tasks_)
        return;
    internal_bus_ = std::make_unique<EventBus>();
    internal_tasks_ = std::make_unique<TaskService>(*internal_bus_);
    tasks_ = internal_tasks_.get();
}

TaskOwner SubAgentExecutor::resolve_owner(const SpawnRequest& request) const {
    if (!request.owner.owner_id.empty())
        return request.owner;
    return task_owner_from_scope();
}

std::string SubAgentExecutor::create_worker_record(const SpawnRequest& request,
                                                   const TaskOwner& owner, std::string& err) {
    TaskRecord record;
    record.kind = TaskKind::Worker;
    record.owner = owner;
    record.label =
        clip(request.label.empty() ? head_of(request.goal, 80) : request.label, kLabelCap);
    record.goal = request.goal;
    record.cwd = owner.worktree.empty() ? owner.project_root : owner.worktree;
    record.notify = TaskNotify::Auto;
    return tasks_->create(std::move(record), &err);
}

void SubAgentExecutor::enqueue_work(const std::string& task_id, SpawnRequest request,
                                    ToolRegistry& reg) {
    Work work;
    work.task_id = task_id;
    work.request = std::move(request);
    work.registry = &reg;
    {
        std::scoped_lock lk(queue_mutex_);
        tokens_[task_id] = CancellationToken{};
        queue_.push_back(std::move(work));
    }
    queue_cv_.notify_all();
}

std::string SubAgentExecutor::spawn(SpawnRequest request, ToolRegistry& reg, std::string& err) {
    err.clear();
    if (t_in_subagent) {
        err = "delegate cannot be nested inside a worker";
        return {};
    }
    if (!validate_request(request, err))
        return {};
    ensure_tasks();

    const TaskOwner owner = resolve_owner(request);
    request.owner = owner;
    std::string id = create_worker_record(request, owner, err);
    if (id.empty())
        return {};
    if (const std::string path = transcript_path(id); !path.empty())
        tasks_->set_artifact(id, path);
    enqueue_work(id, std::move(request), reg);
    tasks_->attach_cancel(id, [this, id](const CancellationToken&) { return cancel_work(id); });
    launched_.fetch_add(1);
    ensure_workers();
    return id;
}

bool SubAgentExecutor::cancel_work(const std::string& task_id) {
    CancellationToken token;
    bool running = false;
    {
        std::scoped_lock lk(queue_mutex_);
        auto it = tokens_.find(task_id);
        if (it == tokens_.end())
            return true; // already finished; the terminal transition wins
        token = it->second;
        running = running_.count(task_id) > 0;
    }
    token.request();
    if (!running)
        return true; // a queued worker is skipped by its consumer

    std::unique_lock lk(queue_mutex_);
    const auto stopped = idle_cv_.wait_for(lk, std::chrono::milliseconds(kCancelWaitMs),
                                           [&] { return running_.count(task_id) == 0; });
    return stopped;
}

void SubAgentExecutor::append_tail(const std::string& task_id, const std::string& line) {
    std::scoped_lock lk(tails_mutex_);
    append_task_tail(tails_[task_id], line);
}

std::string SubAgentExecutor::take_tail(const std::string& task_id) {
    std::scoped_lock lk(tails_mutex_);
    auto it = tails_.find(task_id);
    if (it == tails_.end())
        return {};
    std::string tail = std::move(it->second);
    tails_.erase(it);
    return tail;
}

AgentHooks SubAgentExecutor::progress_hooks(const std::string& task_id) {
    std::string owner;
    std::string label;
    if (tasks_) {
        if (const auto record = tasks_->get(task_id)) {
            owner = record->owner.owner_id;
            label = record->label;
        }
    }
    AgentHooks hooks = sub_hooks_of(owner);
    if (hooks.on_approval) {
        // A gated tool in the child reports through the owner's hook. Record
        // the wait on the task so /tasks can say why the worker is stuck, and
        // the decision once it lands.
        const auto inner = hooks.on_approval;
        hooks.on_approval = [this, task_id, label, inner](const std::string& name, const json& args,
                                                          const std::string& summary) {
            if (tasks_)
                tasks_->progress(task_id, "awaiting approval: " + name);
            append_tail(task_id, "awaiting approval: " + name);
            const std::string labelled = "worker \"" + label + "\": " + summary;
            const Approval approval = inner(name, args, labelled);
            if (tasks_)
                tasks_->progress(task_id, "running");
            append_tail(task_id, std::string("approval: ") + approval_result(approval));
            return approval;
        };
    }
    hooks.on_tool_call = [this, task_id](const std::string& name, const json&) {
        if (tasks_)
            tasks_->progress(task_id, "tool: " + name);
        append_tail(task_id, "-> " + name);
    };
    hooks.on_tool_result = [this, task_id](const std::string& name, const ToolResult& result,
                                           const json&) {
        if (tasks_)
            tasks_->progress(task_id, "running");
        append_tail(task_id, "<- " + name + (result.ok ? " ok" : " error"));
    };
    hooks.on_stats = [this, task_id](const Stats& stats) { record_usage(task_id, stats); };
    return hooks;
}

AgentHooks SubAgentExecutor::sub_hooks_of() const {
    return sub_hooks_of({});
}

AgentHooks SubAgentExecutor::sub_hooks_of(const std::string& owner_id) const {
    const AgentHooks source = (hooks_for_ && !owner_id.empty()) ? hooks_for_(owner_id) : hooks_;
    AgentHooks sub_hooks;
    sub_hooks.on_approval = source.on_approval;
    sub_hooks.on_status = source.on_status;
    sub_hooks.on_api_key = source.on_api_key;
    return sub_hooks;
}

Config SubAgentExecutor::sub_config() const {
    Config sub_cfg = cfg_;
    sub_cfg.cancel_token = CancellationToken{};
    if (sub_cfg.max_tool_iterations <= 0 || sub_cfg.max_tool_iterations > max_iterations_.load())
        sub_cfg.max_tool_iterations = max_iterations_.load();
    return sub_cfg;
}

std::string SubAgentExecutor::run_sub_agent(ToolRegistry& reg, const Config& sub_cfg,
                                            const AgentHooks& sub_hooks,
                                            const SpawnRequest& request, std::string& err,
                                            std::string* transcript) {
    try {
        // The sub shares the parent's registry, where the skill tools are
        // already installed. Its own runs resolve this sub's catalog through
        // the run scope, so nothing is rebound here and nothing can dangle.
        // Its system prompt is rendered by the Agent itself — the same assembly
        // as the parent's, plus the worker directive for running in a sub.
        Agent sub(sub_cfg, reg, sub_hooks, {}, {}, {}, {}, {}, factory_);
        sub.set_project_root(request.owner.project_root);
        sub.set_workspace_root(request.owner.worktree);
        sub.set_owner_id(request.owner.owner_id);
        const std::string reply = sub.run(compose_prompt(request));
        if (transcript)
            *transcript = serialize_context(sub.context());
        return reply;
    } catch (const std::exception& e) {
        err = std::string("sub-agent failed: ") + e.what();
        return {};
    }
}

void SubAgentExecutor::record_usage(const std::string& task_id, const Stats& stats) {
    UsageSample sample;
    sample.source = UsageSource::Subagent;
    sample.prompt_tokens = stats.prompt_tokens;
    sample.completion_tokens = stats.completion_tokens;
    sample.cache_read_tokens = stats.cache_read_tokens;
    sample.cache_write_tokens = stats.cache_write_tokens;
    sample.task_id = task_id;
    if (tasks_) {
        tasks_->add_usage(task_id, stats.prompt_tokens, stats.completion_tokens, stats.valid);
        if (const auto record = tasks_->get(task_id))
            sample.owner_id = record->owner.owner_id;
    }
    if (stats.valid && usage_sink_)
        usage_sink_(sample);
}

bool SubAgentExecutor::begin_work(const Work& work, const CancellationToken& token) {
    if (token.is_requested())
        return false;
    if (!tasks_)
        return true;
    tasks_->started(work.task_id, now_ms());
    tasks_->progress(work.task_id, "starting");
    const auto record = tasks_->get(work.task_id);
    return record && !task_state_terminal(record->state) && !token.is_requested();
}

std::string SubAgentExecutor::execute_child(const Work& work, const CancellationToken& token,
                                            std::string& err, std::string* transcript) {
    Config sub_cfg = sub_config();
    sub_cfg.cancel_token = token;
    if (work.request.max_turns > 0)
        sub_cfg.max_tool_iterations = work.request.max_turns;
    const AgentHooks sub_hooks = progress_hooks(work.task_id);
    t_in_subagent = true;
    const std::string reply =
        run_sub_agent(*work.registry, sub_cfg, sub_hooks, work.request, err, transcript);
    t_in_subagent = false;
    return reply;
}

void SubAgentExecutor::finish_work(const Work& work, const CancellationToken& token,
                                   const std::string& reply, const std::string& err,
                                   std::string transcript) {
    const std::string path = transcript_path(work.task_id);
    if (!path.empty()) {
        const std::string dir = artifact_root_ + "/" + work.task_id;
        if (ensure_private_dir(dir)) {
            if (transcript.empty()) {
                Message message;
                message.role = "assistant";
                message.content = err.empty() ? reply : err;
                transcript = message_line(message) + "\n";
            }
            write_private_file(path, transcript);
        }
    }

    TaskResult result;
    result.exit_code = err.empty() ? 0 : 1;
    result.summary = clip(err.empty() ? reply : err, kReportCap);
    result.output_tail = take_tail(work.task_id);
    result.artifact_path = path;
    if (!tasks_)
        return;
    if (token.is_requested()) {
        // cancel() owns the terminal transition; the artifact already carries
        // whatever the child produced before it stopped.
        tasks_->progress(work.task_id, "cancelled");
        return;
    }
    tasks_->finish(work.task_id, err.empty() ? TaskState::Succeeded : TaskState::Failed,
                   std::move(result), err);
}

void SubAgentExecutor::cleanup_work(const std::string& task_id) {
    {
        std::scoped_lock lk(queue_mutex_);
        running_.erase(task_id);
        tokens_.erase(task_id);
    }
    idle_cv_.notify_all();
}

void SubAgentExecutor::run_work(const Work& work) {
    CancellationToken token;
    {
        std::scoped_lock lk(queue_mutex_);
        auto it = tokens_.find(work.task_id);
        if (it == tokens_.end())
            return;
        token = it->second;
    }
    if (!begin_work(work, token)) {
        cleanup_work(work.task_id);
        return;
    }

    std::string err;
    std::string transcript;
    const std::string reply = execute_child(work, token, err, &transcript);
    finish_work(work, token, reply, err, std::move(transcript));
    cleanup_work(work.task_id);
}

void SubAgentExecutor::worker_loop() {
    for (;;) {
        Work work;
        {
            std::unique_lock lk(queue_mutex_);
            queue_cv_.wait(lk, [&] { return shutting_down_ || !queue_.empty(); });
            if (queue_.empty())
                return; // shutting down with nothing left to start
            work = std::move(queue_.front());
            queue_.pop_front();
            running_.insert(work.task_id);
        }
        run_work(work);
    }
}

std::string SubAgentExecutor::run_task(const std::string& prompt, ToolRegistry& reg,
                                       std::string& err) {
    SpawnRequest request;
    request.goal = prompt;
    request.context = "This is a focused task from the parent conversation.";
    request.expect = "Reply with a concise report of what you did and found.";
    request.owner = task_owner_from_scope();
    const std::string id = spawn(std::move(request), reg, err);
    if (id.empty())
        return {};
    const CancellationToken token;
    const auto record = tasks_->wait_for_terminal(id, 24 * 60 * 60 * 1000, token);
    if (!record) {
        err = "delegate: worker did not finish";
        return {};
    }
    if (record->state != TaskState::Succeeded) {
        err = record->error.empty() ? record->result.summary : record->error;
        return {};
    }
    return record->result.summary;
}

void SubAgentExecutor::shutdown() {
    std::vector<std::string> live;
    {
        std::scoped_lock lk(queue_mutex_);
        if (shutting_down_ && workers_.empty())
            return;
        shutting_down_ = true;
        for (const auto& work : queue_)
            live.push_back(work.task_id);
        for (const auto& id : running_)
            live.push_back(id);
    }
    if (tasks_) {
        for (const auto& id : live)
            tasks_->cancel(id, "executor shut down", cfg_.cancel_token);
    }
    queue_cv_.notify_all();
    for (auto& worker : workers_) {
        if (worker.joinable())
            worker.join();
    }
    workers_.clear();
    {
        std::scoped_lock lk(queue_mutex_);
        worker_count_ = 0;
        queue_.clear();
        running_.clear();
        tokens_.clear();
    }
}

} // namespace agent
