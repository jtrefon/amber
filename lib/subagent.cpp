
#include "agent/subagent.h"

#include <chrono>

#include <algorithm>

#include "agent/agent.h"
#include "agent/run_scope.h"

namespace agent {

namespace {

thread_local bool t_in_subagent = false;
thread_local bool t_subagent_inherited = false;

} // namespace

std::string worker_directive() {
    return "## Worker directive\n\n"
           "You are a focused worker agent executing one task on behalf of "
           "the main agent. The task below is yours alone — complete it with "
           "the available tools, working autonomously. When the task is "
           "complete, reply with a concise report: what you did, what you "
           "found, and what remains if anything.";
}

bool in_subagent() noexcept {
    return t_in_subagent || t_subagent_inherited;
}

void set_subagent_inherited(bool value) noexcept {
    t_subagent_inherited = value;
}

bool SubAgentExecutor::acquire_slot() {
    std::unique_lock<std::mutex> lk(slot_mutex_);
    // Bounded wait: a task that cannot get a slot reports back rather than
    // waiting forever. An unbounded wait here hangs the parent for as long as
    // the slots stay taken — which is exactly what "a task that never finishes"
    // looks like from outside.
    if (!slot_cv_.wait_for(lk, std::chrono::milliseconds(slot_wait_ms_.load()),
                           [this] { return active_ < max_.load(); }))
        return false;
    ++active_;
    return true;
}

void SubAgentExecutor::release_slot() noexcept {
    {
        std::scoped_lock lk(slot_mutex_);
        --active_;
    }
    slot_cv_.notify_one();
}

int SubAgentExecutor::active() const noexcept {
    std::scoped_lock lk(slot_mutex_);
    return active_;
}

std::string SubAgentExecutor::add_task(const std::string& prompt) {
    TaskRecord record;
    record.id = "task-" + std::to_string(++next_task_);
    if (const auto* scope = current_run_scope();
        scope && scope->leaf.owner_id && !scope->leaf.owner_id->empty())
        record.owner_id = *scope->leaf.owner_id;
    record.prompt_head = prompt.size() > 100 ? prompt.substr(0, 97) + "..." : prompt;
    record.state = "queued";
    record.started = std::chrono::steady_clock::now();
    std::scoped_lock lk(tasks_mutex_);
    if (tasks_.size() >= 64) {
        const auto done = std::find_if(tasks_.begin(), tasks_.end(), [](const TaskRecord& r) {
            return r.state != "running" && r.state != "queued";
        });
        if (done != tasks_.end())
            tasks_.erase(done);
        else
            tasks_.erase(tasks_.begin());
    }
    tasks_.push_back(std::move(record));
    return tasks_.back().id;
}

void SubAgentExecutor::update_task(const std::string& id, const std::string& state,
                                   const std::string& current_tool, const std::string& result) {
    std::scoped_lock lk(tasks_mutex_);
    for (auto& record : tasks_) {
        if (record.id != id)
            continue;
        record.state = state;
        if (!current_tool.empty())
            record.current_tool = current_tool;
        else if (state == "running")
            record.current_tool.clear();
        if (!result.empty())
            record.result = result.size() > 400 ? result.substr(0, 397) + "..." : result;
        return;
    }
}

void SubAgentExecutor::fail_task(const std::string& id, const std::string& error) {
    waiting_.fetch_sub(1);
    update_task(id, "failed", {}, error);
}

std::vector<SubAgentTaskInfo> SubAgentExecutor::tasks() const {
    std::scoped_lock lk(tasks_mutex_);
    std::vector<SubAgentTaskInfo> out;
    const auto now = std::chrono::steady_clock::now();
    for (const auto& record : tasks_) {
        SubAgentTaskInfo info;
        info.id = record.id;
        info.owner_id = record.owner_id;
        info.prompt_head = record.prompt_head;
        info.state = record.state;
        info.current_tool = record.current_tool;
        info.result = record.result;
        info.prompt_tokens = record.prompt_tokens;
        info.completion_tokens = record.completion_tokens;
        info.usage_measured = record.usage_measured;
        info.elapsed_s =
            std::chrono::duration_cast<std::chrono::seconds>(now - record.started).count();
        info.slot = record.slot;
        out.push_back(std::move(info));
    }
    return out;
}

// Sub-agent hooks: approval and status passthrough only — tool calls, tokens and
// state of the worker never leak into the parent's observers.
AgentHooks SubAgentExecutor::sub_hooks_of() const {
    AgentHooks sub_hooks;
    sub_hooks.on_approval = hooks_.on_approval;
    sub_hooks.on_status = hooks_.on_status;
    sub_hooks.on_api_key = hooks_.on_api_key;
    return sub_hooks;
}

// The sub-agent's config: a fresh cancel flag and the iteration cap. Sibling
// sub-agents of one parent must not share the flag (cancelling one would kill
// the other); parent cancel still reaches a sub through the RunScope ancestor
// chain.
Config SubAgentExecutor::sub_config() const {
    Config sub_cfg = cfg_;
    sub_cfg.cancel_token = CancellationToken{};
    if (sub_cfg.max_tool_iterations <= 0 || sub_cfg.max_tool_iterations > max_iterations_.load())
        sub_cfg.max_tool_iterations = max_iterations_.load();
    return sub_cfg;
}

// Serial mode: one sub-agent at a time (cache-friendly request ordering).
// Bounded like the slot wait, so a wedged sibling cannot hold the parent
// forever. Returns false with `err` set when the wait expires.
bool SubAgentExecutor::enter_serial_mode(std::unique_lock<std::timed_mutex>& guard,
                                         std::string& err) {
    if (parallel_.load())
        return true;
    if (guard.try_lock_for(std::chrono::milliseconds(slot_wait_ms_.load())))
        return true;
    err = "another sub-agent is still running; retry shortly";
    return false;
}

std::string SubAgentExecutor::run_sub_agent(const std::string& prompt, ToolRegistry& reg,
                                            const Config& sub_cfg, const AgentHooks& sub_hooks,
                                            std::string& err) {
    try {
        // The sub shares the parent's registry, where the skill tools are
        // already installed. Its own runs resolve this sub's catalog through
        // the run scope, so nothing is rebound here and nothing can dangle.
        // Its system prompt is rendered by the Agent itself — the same assembly
        // as the parent's, plus the worker directive for running in a sub.
        Agent sub(sub_cfg, reg, sub_hooks, {}, {}, {}, {}, {}, factory_);
        return sub.run(prompt);
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
    sample.task_id = task_id;
    {
        std::scoped_lock lk(tasks_mutex_);
        for (auto& record : tasks_) {
            if (record.id != task_id)
                continue;
            if (stats.prompt_tokens >= 0) {
                record.prompt_tokens += stats.prompt_tokens;
                record.usage_measured = true;
            }
            if (stats.completion_tokens >= 0) {
                record.completion_tokens += stats.completion_tokens;
                record.usage_measured = true;
            }
            sample.owner_id = record.owner_id;
            break;
        }
    }
    if (stats.valid && usage_sink_)
        usage_sink_(sample);
}

AgentHooks SubAgentExecutor::progress_hooks(const std::string& task_id) {
    AgentHooks hooks = sub_hooks_of();
    hooks.on_tool_call = [this, task_id](const std::string& name, const json&) {
        update_task(task_id, "running", name);
    };
    hooks.on_tool_result = [this, task_id](const std::string&, const ToolResult&, const json&) {
        update_task(task_id, "running");
    };
    hooks.on_stats = [this, task_id](const Stats& stats) { record_usage(task_id, stats); };
    return hooks;
}

std::string SubAgentExecutor::run_task(const std::string& prompt, ToolRegistry& reg,
                                       std::string& err) {
    err.clear();
    if (t_in_subagent) {
        err = "task cannot be nested inside a sub-agent";
        return "";
    }

    const std::string task_id = add_task(prompt);
    waiting_.fetch_add(1);
    std::unique_lock<std::timed_mutex> serial_guard(serial_mutex_, std::defer_lock);
    if (!enter_serial_mode(serial_guard, err)) {
        fail_task(task_id, err);
        return "";
    }
    if (!acquire_slot()) {
        err = "all sub-agent slots are busy; retry shortly";
        fail_task(task_id, err);
        return "";
    }
    waiting_.fetch_sub(1);
    struct SlotGuard {
        SubAgentExecutor* self;
        ~SlotGuard() { self->release_slot(); }
    } slot_guard{this};

    update_task(task_id, "running");
    const AgentHooks sub_hooks = progress_hooks(task_id);
    const Config sub_cfg = sub_config();

    launched_.fetch_add(1);
    t_in_subagent = true;
    const std::string result = run_sub_agent(prompt, reg, sub_cfg, sub_hooks, err);
    t_in_subagent = false;
    update_task(task_id, err.empty() ? "done" : "failed", {}, result);
    return result;
}

} // namespace agent
