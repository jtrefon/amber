// The tasks tool is the one lifecycle surface over TaskService: one owner
// scope, one bounded output stream, one cancellation path and one bounded wait.

#include "agent/events.h"
#include "agent/job.h"
#include "agent/run_scope.h"
#include "agent/task.h"
#include "agent/tools.h"
#include "agent/workspace.h"
#include "tests/test_util.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

namespace {

namespace fs = std::filesystem;

agent::TaskRecord task_record(const std::string& owner, const std::string& label) {
    agent::TaskRecord record;
    record.kind = agent::TaskKind::Command;
    record.owner.owner_id = owner;
    record.label = label;
    return record;
}

bool wait_until_done(agent::JobService& jobs, const std::string& id) {
    for (int i = 0; i < 500; ++i) {
        auto job = jobs.get(id);
        if (!job || job->is_done())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

fs::path tasks_workspace() {
    const fs::path dir = fs::temp_directory_path() / "amber_tasks_tool_test";
    fs::create_directories(dir);
    return dir;
}

struct WorkspaceGuard {
    explicit WorkspaceGuard(const std::string& root) : saved(agent::Workspace::workspace_root()) {
        agent::Workspace::set_root(root);
    }
    ~WorkspaceGuard() { agent::Workspace::set_root(saved); }
    std::string saved;
};

} // namespace

TEST(process_start_registers_a_task) {
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    agent::JobService jobs;
    auto start = agent::make_process_start_tool(jobs, &tasks);

    std::string owner = "window-a";
    agent::RunScope scope;
    scope.owner_id = &owner;
    agent::ScopedRunScope guard(scope);

    auto started = start->execute({{"command", "sh -c 'exit 3'"}, {"label", "exit three"}});
    ASSERT(started.ok);
    const std::string id = started.output;
    ASSERT_FALSE(id.empty());
    ASSERT(tasks.get(id).has_value());
    ASSERT(tasks.get(id)->owner.owner_id == owner);
    ASSERT(tasks.get(id)->label == std::string("exit three"));
    ASSERT_EQ(tasks.backend_id(id), started.meta["job_id"].get<std::string>());

    auto final = tasks.wait_for_terminal(id, 2000, agent::CancellationToken{});
    ASSERT(final.has_value());
    ASSERT(final->state == agent::TaskState::Failed);
    ASSERT_EQ(final->result.exit_code, 3);
}

TEST(bash_inactive_handoff_finishes_the_task) {
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    agent::JobService jobs;
    agent::CancellationToken token;
    auto bash = agent::make_bash_tool(&jobs, token, &tasks);
    auto tool = agent::make_tasks_tool(tasks, jobs);

    std::string owner = "window-a";
    agent::RunScope scope;
    scope.owner_id = &owner;
    agent::ScopedRunScope guard(scope);

    auto handoff = bash->execute({{"command", "sleep 2; echo finished"}, {"timeout", 1}});
    ASSERT(handoff.ok);
    const std::string id = handoff.meta.value("task", std::string(""));
    ASSERT_FALSE(id.empty());
    ASSERT(tasks.backend_id(id) == handoff.meta.value("job", std::string("")));

    auto final = tasks.wait_for_terminal(id, 5000, token);
    ASSERT(final.has_value());
    ASSERT(final->state == agent::TaskState::Succeeded);

    auto output = tool->execute({{"action", "output"}, {"id", id}});
    ASSERT(output.ok);
    ASSERT(output.output.find("finished") != std::string::npos);
}

TEST(tasks_list_scopes_by_owner) {
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    agent::JobService jobs;
    auto tool = agent::make_tasks_tool(tasks, jobs);
    const std::string mine = tasks.create(task_record("window-a", "build"));
    const std::string theirs = tasks.create(task_record("window-b", "test"));

    std::string owner = "window-a";
    agent::RunScope scope;
    scope.owner_id = &owner;
    agent::ScopedRunScope guard(scope);

    auto result = tool->execute({{"action", "list"}});
    ASSERT(result.ok);
    ASSERT(result.output.find(mine) != std::string::npos);
    ASSERT(result.output.find(theirs) == std::string::npos);
    ASSERT_EQ(result.meta["count"], 1);
}

TEST(tasks_status_returns_the_record) {
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    agent::JobService jobs;
    auto tool = agent::make_tasks_tool(tasks, jobs);
    const std::string id = tasks.create(task_record("window-a", "build the project"));
    ASSERT_TRUE(tasks.started(id, 1));

    std::string owner = "window-a";
    agent::RunScope scope;
    scope.owner_id = &owner;
    agent::ScopedRunScope guard(scope);

    auto result = tool->execute({{"action", "status"}, {"id", id}});
    ASSERT(result.ok);
    ASSERT(result.output.find("state: running") != std::string::npos);
    ASSERT(result.output.find("label: build the project") != std::string::npos);
    ASSERT(result.output.find("owner: window-a") != std::string::npos);
}

TEST(tasks_status_hides_another_windows_task) {
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    agent::JobService jobs;
    auto tool = agent::make_tasks_tool(tasks, jobs);
    const std::string theirs = tasks.create(task_record("window-b", "private"));

    std::string owner = "window-a";
    agent::RunScope scope;
    scope.owner_id = &owner;
    agent::ScopedRunScope guard(scope);

    auto result = tool->execute({{"action", "status"}, {"id", theirs}});
    ASSERT_FALSE(result.ok);
    ASSERT(result.error.find("no such task") != std::string::npos);
}

TEST(tasks_output_is_bounded_and_pages_with_since) {
    const fs::path root = tasks_workspace();
    WorkspaceGuard guard_root(root.string());
    agent::JobService jobs;
    const std::string job_id = jobs.start("yes 0123456789 | head -c 20000", root.string(), 5, 5);
    ASSERT_FALSE(job_id.empty());
    ASSERT_TRUE(wait_until_done(jobs, job_id));

    agent::EventBus bus;
    agent::TaskService tasks(bus);
    auto tool = agent::make_tasks_tool(tasks, jobs);
    const std::string id = tasks.create(task_record("window-a", "big output"));
    ASSERT_TRUE(tasks.started(id, 1));
    ASSERT_TRUE(tasks.bind_backend(id, job_id));

    std::string owner = "window-a";
    agent::RunScope scope;
    scope.owner_id = &owner;
    agent::ScopedRunScope guard(scope);

    auto first = tool->execute({{"action", "output"}, {"id", id}});
    ASSERT(first.ok);
    ASSERT_EQ(first.meta["next_since"], 8192);
    ASSERT_EQ(first.meta["truncated"], true);

    auto second = tool->execute({{"action", "output"}, {"id", id}, {"since", 8192}});
    ASSERT(second.ok);
    ASSERT_EQ(second.meta["next_since"], 16384);
    ASSERT_EQ(second.meta["truncated"], true);

    auto last = tool->execute({{"action", "output"}, {"id", id}, {"since", 16384}});
    ASSERT(last.ok);
    ASSERT(last.meta["next_since"].get<std::size_t>() >= 20000u);
    ASSERT_EQ(last.meta["truncated"], false);

    jobs.stop(job_id);
}

TEST(tasks_cancel_command_kills_the_process_group) {
    const fs::path root = tasks_workspace();
    WorkspaceGuard guard_root(root.string());
    agent::JobService jobs;
    const std::string job_id = jobs.start("sleep 30", root.string(), 600, 600);
    ASSERT_FALSE(job_id.empty());

    agent::EventBus bus;
    agent::TaskService tasks(bus);
    auto tool = agent::make_tasks_tool(tasks, jobs);
    const std::string id = tasks.create(task_record("window-a", "long command"));
    ASSERT_TRUE(tasks.started(id, 1));
    ASSERT_TRUE(tasks.bind_backend(id, job_id));
    ASSERT_TRUE(tasks.attach_cancel(
        id, [&jobs, job_id](const agent::CancellationToken&) { return jobs.stop(job_id); }));

    std::string owner = "window-a";
    agent::RunScope scope;
    scope.owner_id = &owner;
    agent::ScopedRunScope guard(scope);

    auto result = tool->execute({{"action", "cancel"}, {"id", id}});
    ASSERT(result.ok);
    ASSERT(result.output.find("state: cancelled") != std::string::npos);
    ASSERT_EQ(jobs.running_count(), 0);
    ASSERT(tasks.get(id)->state == agent::TaskState::Cancelled);
}

TEST(tasks_cancel_unknown_is_an_error) {
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    agent::JobService jobs;
    auto tool = agent::make_tasks_tool(tasks, jobs);

    std::string owner = "window-a";
    agent::RunScope scope;
    scope.owner_id = &owner;
    agent::ScopedRunScope guard(scope);

    auto result = tool->execute({{"action", "cancel"}, {"id", "task_missing"}});
    ASSERT_FALSE(result.ok);
    ASSERT(result.error.find("no such task") != std::string::npos);
}

TEST(tasks_wait_is_bounded_and_returns_the_terminal_record) {
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    agent::JobService jobs;
    auto tool = agent::make_tasks_tool(tasks, jobs);
    const std::string id = tasks.create(task_record("window-a", "wait for me"));
    ASSERT_TRUE(tasks.started(id, 1));

    std::string owner = "window-a";
    agent::RunScope scope;
    scope.owner_id = &owner;
    agent::ScopedRunScope guard(scope);

    auto timed_out = tool->execute({{"action", "wait"}, {"id", id}, {"timeout_s", 1}});
    ASSERT(timed_out.ok);
    ASSERT(timed_out.output.find("still running") != std::string::npos);

    std::thread finish([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        tasks.finish(id, agent::TaskState::Succeeded, {});
    });
    auto done = tool->execute({{"action", "wait"}, {"id", id}, {"timeout_s", 2}});
    finish.join();
    ASSERT(done.ok);
    ASSERT(done.output.find("state: succeeded") != std::string::npos);
}

TEST(tasks_wait_is_cancellable) {
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    agent::JobService jobs;
    auto tool = agent::make_tasks_tool(tasks, jobs);
    const std::string id = tasks.create(task_record("window-a", "cancel the wait"));
    ASSERT_TRUE(tasks.started(id, 1));

    agent::CancellationToken token;
    std::string owner = "window-a";
    agent::RunScope scope;
    scope.owner_id = &owner;
    scope.cancel_token = &token;
    agent::ScopedRunScope guard(scope);

    std::thread cancel([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        token.request();
    });
    auto result = tool->execute({{"action", "wait"}, {"id", id}, {"timeout_s", 10}});
    cancel.join();
    ASSERT_FALSE(result.ok);
    ASSERT(result.error.find("cancelled") != std::string::npos);
    tasks.finish(id, agent::TaskState::Succeeded, {});
}
