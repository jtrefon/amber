
#include "agent/subagent.h"

#include <algorithm>

#include "agent/agent.h"
#include "agent/environment.h"
#include "agent/prompt.h"

namespace agent {

namespace {

thread_local bool t_in_subagent = false;
thread_local bool t_subagent_inherited = false;

// Sub-agent system prompt: the base system prompt + environment card, plus a
// worker directive describing the focused role and the report contract.
std::string compose_subagent_system(const Config& cfg) {
    std::string sys = load_prompt(cfg.system_prompt_path.empty()
                                      ? "prompts/system.md"
                                      : cfg.system_prompt_path);
    const std::string env_card = render_environment_card(probe_environment());
    if (!env_card.empty()) sys += "\n\n" + env_card;
    sys += "\n\n## Worker directive\n\n"
           "You are a focused worker agent executing one task on behalf of "
           "the main agent. The task below is yours alone — complete it with "
           "the available tools, working autonomously. When the task is "
           "complete, reply with a concise report: what you did, what you "
           "found, and what remains if anything.";
    return sys;
}

} // namespace

bool in_subagent() noexcept {
    return t_in_subagent || t_subagent_inherited;
}

void set_subagent_inherited(bool value) noexcept {
    t_subagent_inherited = value;
}

bool SubAgentExecutor::acquire_slot() {
    std::unique_lock<std::mutex> lk(slot_mutex_);
    slot_cv_.wait(lk, [this] { return active_ < max_.load(); });
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

std::string SubAgentExecutor::run_task(const std::string& prompt,
                                       ToolRegistry& reg, std::string& err) {
    err.clear();
    if (t_in_subagent) {
        err = "task cannot be nested inside a sub-agent";
        return "";
    }

    // Serial mode: one sub-agent at a time (cache-friendly request ordering).
    std::unique_lock<std::mutex> serial_guard(serial_mutex_,
                                              std::defer_lock);
    if (!parallel_.load()) serial_guard.lock();
    acquire_slot();
    struct SlotGuard {
        SubAgentExecutor* self;
        ~SlotGuard() { self->release_slot(); }
    } slot_guard{this};

    // Sub-agent hooks: approval and status passthrough only — tool calls,
    // tokens and state of the worker never leak into the parent's observers.
    AgentHooks sub_hooks;
    sub_hooks.on_approval = hooks_.on_approval;
    sub_hooks.on_status = hooks_.on_status;

    Config sub_cfg = cfg_;
    if (sub_cfg.max_tool_iterations <= 0 ||
        sub_cfg.max_tool_iterations > max_iterations_.load())
        sub_cfg.max_tool_iterations = max_iterations_.load();

    Message sys;
    sys.role = "system";
    sys.content = compose_subagent_system(sub_cfg);

    std::string result;
    launched_.fetch_add(1);
    t_in_subagent = true;
    try {
        // Skill tools stay with the parent session: registering them here
        // would bind read_skill/list_skills/write_skill to this sub's
        // SkillCatalog, replace the parent's bindings in the shared registry,
        // and dangle once the sub is destroyed.
        Agent sub(sub_cfg, reg, sub_hooks, {}, {}, {}, {}, {}, factory_,
                  false);
        sub.set_context({std::move(sys)});
        result = sub.run(prompt);
    } catch (const std::exception& e) {
        err = std::string("sub-agent failed: ") + e.what();
    }
    t_in_subagent = false;
    return result;
}

} // namespace agent
