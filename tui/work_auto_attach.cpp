#include "work_auto_attach.h"

namespace tui {

namespace {

bool job_running(agent::JobState state) {
    return state == agent::JobState::Running || state == agent::JobState::Starting;
}

bool task_running(const std::string& state) {
    return state == "running" || state == "queued";
}

struct PlanInput {
    const std::unordered_set<std::string>& attached;
    const std::function<bool(const std::string&, const std::string&)>& ever_focused;
};

void plan_one(const std::string& kind, const std::string& id, const std::string& title, bool live,
              const PlanInput& in, std::vector<AutoAttachStep>& out) {
    const std::string key = kind + ":" + id;
    if (live) {
        if (in.attached.count(key) == 0)
            out.push_back({AutoAttachStep::Action::Open, kind, id, title, key});
        return;
    }
    if (in.attached.count(key) == 0)
        return;
    const auto action =
        in.ever_focused(kind, id) ? AutoAttachStep::Action::Forget : AutoAttachStep::Action::Close;
    out.push_back({action, kind, id, {}, key});
}

} // namespace

std::vector<AutoAttachStep>
plan_auto_attach(const std::string& mode, const std::vector<agent::JobInfo>& jobs,
                 const std::vector<agent::SubAgentTaskInfo>& tasks,
                 const std::unordered_set<std::string>& attached,
                 const std::function<bool(const std::string&, const std::string&)>& ever_focused) {
    std::vector<AutoAttachStep> steps;
    if (mode == "off")
        return steps;
    const PlanInput in{attached, ever_focused};
    if (mode == "jobs" || mode == "all") {
        for (const auto& job : jobs)
            plan_one("job", job.id, "job " + job.id + " \u2014 " + job.label,
                     job_running(job.state), in, steps);
    }
    if (mode == "agents" || mode == "all") {
        for (const auto& task : tasks)
            plan_one("agent", task.id, "agent " + task.id + " \u2014 " + task.prompt_head,
                     task_running(task.state), in, steps);
    }
    return steps;
}

} // namespace tui
