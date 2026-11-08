#include "agent/events.h"
#include "agent/job.h"
#include "agent/task.h"
#include "agent/task_join.h"
#include "agent/task_store.h"
#include "agent/workspace.h"
#include "tests/test_util.h"

#include <atomic>
#include <chrono>
#include <set>
#include <string>
#include <thread>

// The failure message prints the record's enums by name.
namespace agent {
inline std::ostream& operator<<(std::ostream& os, TaskState state) {
    return os << task_state_name(state);
}
inline std::ostream& operator<<(std::ostream& os, TaskKind kind) {
    return os << task_kind_name(kind);
}
} // namespace agent

namespace {

agent::TaskRecord make_record(agent::TaskKind kind, const std::string& owner,
                              const std::string& label) {
    agent::TaskRecord record;
    record.kind = kind;
    record.owner.owner_id = owner;
    record.label = label;
    return record;
}

agent::TaskLimits roomy_limits() {
    agent::TaskLimits limits;
    limits.command_per_owner = 100000;
    limits.command_global = 100000;
    limits.watch_per_owner = 100000;
    limits.watch_global = 100000;
    limits.worker_per_owner = 100000;
    limits.worker_global = 100000;
    limits.total_per_owner = 100000;
    limits.total_global = 100000;
    return limits;
}

} // namespace

TEST(task_service_creates_queued_record_with_unique_id) {
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    const std::string id = tasks.create(make_record(agent::TaskKind::Command, "w1", "build"));
    ASSERT_FALSE(id.empty());
    auto record = tasks.get(id);
    ASSERT_TRUE(record.has_value());
    ASSERT_EQ(record->state, agent::TaskState::Queued);
    ASSERT_EQ(record->owner.owner_id, std::string("w1"));
    ASSERT_EQ(record->label, std::string("build"));

    const std::string other = tasks.create(make_record(agent::TaskKind::Command, "w1", "test"));
    ASSERT_FALSE(other.empty());
    ASSERT_TRUE(id != other);
}

TEST(task_service_owner_scoping) {
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    tasks.create(make_record(agent::TaskKind::Command, "a", "a1"));
    tasks.create(make_record(agent::TaskKind::Watch, "a", "a2"));
    tasks.create(make_record(agent::TaskKind::Worker, "b", "b1"));

    ASSERT_EQ(tasks.list("a").size(), 2u);
    ASSERT_EQ(tasks.list("b").size(), 1u);
    ASSERT_EQ(tasks.list("missing").size(), 0u);
    ASSERT_EQ(tasks.all().size(), 3u);
}

TEST(task_service_state_machine_rejects_illegal_transitions) {
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    const std::string id = tasks.create(make_record(agent::TaskKind::Command, "w", "x"));

    ASSERT_FALSE(tasks.finish(id, agent::TaskState::Succeeded, {}));
    ASSERT_EQ(tasks.get(id)->state, agent::TaskState::Queued);
    ASSERT_TRUE(tasks.started(id, 1));
    ASSERT_FALSE(tasks.started(id, 2));
    ASSERT_EQ(tasks.get(id)->state, agent::TaskState::Running);
    ASSERT_TRUE(tasks.finish(id, agent::TaskState::Succeeded, {}));
    ASSERT_EQ(tasks.get(id)->state, agent::TaskState::Succeeded);
}

TEST(task_service_terminal_state_is_immutable) {
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    const std::string id = tasks.create(make_record(agent::TaskKind::Command, "w", "x"));
    ASSERT_TRUE(tasks.started(id, 1));
    ASSERT_TRUE(tasks.finish(id, agent::TaskState::Succeeded, {}));
    const auto before = tasks.get(id);
    ASSERT_TRUE(before.has_value());

    ASSERT_FALSE(tasks.started(id, 2));
    ASSERT_FALSE(tasks.progress(id, "late"));
    ASSERT_FALSE(tasks.finish(id, agent::TaskState::Failed, {}));
    agent::CancellationToken token;
    ASSERT_FALSE(tasks.cancel(id, "late", token));
    const auto after = tasks.get(id);
    ASSERT_EQ(after->state, before->state);
    ASSERT_EQ(after->progress, before->progress);
    ASSERT_EQ(after->ended_ms, before->ended_ms);
}

TEST(task_service_caps_per_owner_and_global) {
    agent::EventBus bus;
    agent::TaskLimits limits;
    limits.command_per_owner = 2;
    limits.command_global = 3;
    limits.total_per_owner = 10;
    limits.total_global = 10;
    agent::TaskService tasks(bus, limits);

    ASSERT_FALSE(tasks.create(make_record(agent::TaskKind::Command, "a", "1")).empty());
    ASSERT_FALSE(tasks.create(make_record(agent::TaskKind::Command, "a", "2")).empty());
    ASSERT_TRUE(tasks.create(make_record(agent::TaskKind::Command, "a", "3")).empty());
    ASSERT_FALSE(tasks.create(make_record(agent::TaskKind::Command, "b", "4")).empty());
    // Global command cap is 3, so the next command is refused even for a new
    // owner; a watch still has room.
    ASSERT_TRUE(tasks.create(make_record(agent::TaskKind::Command, "b", "5")).empty());
    ASSERT_FALSE(tasks.create(make_record(agent::TaskKind::Watch, "b", "w")).empty());
}

TEST(task_service_cap_error_says_do_not_retry) {
    agent::EventBus bus;
    agent::TaskLimits limits;
    limits.worker_per_owner = 1;
    limits.worker_global = 4;
    limits.total_per_owner = 10;
    limits.total_global = 10;
    agent::TaskService tasks(bus, limits);
    ASSERT_FALSE(tasks.create(make_record(agent::TaskKind::Worker, "a", "1")).empty());
    std::string error;
    ASSERT_TRUE(tasks.create(make_record(agent::TaskKind::Worker, "a", "2"), &error).empty());
    ASSERT_TRUE(error.find("do not retry") != std::string::npos);
}

TEST(task_service_completion_publishes_one_event) {
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    agent::Events events(bus);
    int started = 0;
    int progress = 0;
    int completed = 0;
    int cancelled = 0;
    auto s0 = events.subscribe<agent::TaskStartedEvent>([&](const auto&) { ++started; });
    auto s1 = events.subscribe<agent::TaskProgressEvent>([&](const auto&) { ++progress; });
    auto s2 = events.subscribe<agent::TaskCompletedEvent>([&](const auto&) { ++completed; });
    auto s3 = events.subscribe<agent::TaskCancelledEvent>([&](const auto&) { ++cancelled; });

    const std::string id = tasks.create(make_record(agent::TaskKind::Command, "w", "x"));
    ASSERT_EQ(started, 0);
    ASSERT_TRUE(tasks.started(id, 1));
    ASSERT_EQ(started, 1);
    ASSERT_TRUE(tasks.progress(id, "one"));
    ASSERT_EQ(progress, 1);
    ASSERT_TRUE(tasks.finish(id, agent::TaskState::Succeeded, {}));
    ASSERT_EQ(completed, 1);
    ASSERT_EQ(cancelled, 0);
}

TEST(task_service_cancel_running_reaches_backend) {
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    const std::string id = tasks.create(make_record(agent::TaskKind::Command, "w", "x"));
    ASSERT_TRUE(tasks.started(id, 1));
    bool called = false;
    ASSERT_TRUE(tasks.attach_cancel(id, [&](const agent::CancellationToken&) {
        called = true;
        return true;
    }));
    agent::CancellationToken token;
    ASSERT_TRUE(tasks.cancel(id, "user", token));
    ASSERT_TRUE(called);
    ASSERT_EQ(tasks.get(id)->state, agent::TaskState::Cancelled);
    ASSERT_EQ(tasks.get(id)->cancel_reason, std::string("user"));
}

// A backend's stop() joins its reader, and that reader may publish a terminal
// completion while the cancel handler is still running. The cancel owns the
// terminal transition; the racing completion is a duplicate, not a second
// opinion, so the record must end Cancelled rather than Interrupted.
TEST(task_service_cancel_wins_over_a_racing_completion) {
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    const std::string id = tasks.create(make_record(agent::TaskKind::Command, "w", "x"));
    ASSERT_TRUE(tasks.started(id, 1));
    ASSERT_TRUE(tasks.attach_cancel(id, [&](const agent::CancellationToken&) {
        // The job's completion sink: it runs while stop() waits for the reader.
        tasks.finish(id, agent::TaskState::Interrupted, {});
        return true;
    }));
    agent::CancellationToken token;
    ASSERT_TRUE(tasks.cancel(id, "stop", token));
    ASSERT_EQ(tasks.get(id)->state, agent::TaskState::Cancelled);
}

TEST(task_service_cancel_unknown_backend_marks_unknown) {
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    const std::string id = tasks.create(make_record(agent::TaskKind::Command, "w", "x"));
    ASSERT_TRUE(tasks.started(id, 1));
    agent::CancellationToken token;
    ASSERT_TRUE(tasks.cancel(id, "no backend", token));
    auto record = tasks.get(id);
    ASSERT_EQ(record->state, agent::TaskState::Unknown);
    ASSERT_TRUE(record->error.find("no backend") != std::string::npos);
}

TEST(task_service_generation_moves_on_every_transition) {
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    const std::uint64_t g0 = tasks.generation();
    const std::string id = tasks.create(make_record(agent::TaskKind::Command, "w", "x"));
    const std::uint64_t g1 = tasks.generation();
    ASSERT_TRUE(g1 > g0);
    ASSERT_TRUE(tasks.started(id, 1));
    const std::uint64_t g2 = tasks.generation();
    ASSERT_TRUE(g2 > g1);
    ASSERT_TRUE(tasks.progress(id, "p"));
    const std::uint64_t g3 = tasks.generation();
    ASSERT_TRUE(g3 > g2);
    ASSERT_TRUE(tasks.finish(id, agent::TaskState::Succeeded, {}));
    ASSERT_TRUE(tasks.generation() > g3);
}

TEST(task_service_id_unique_10k) {
    agent::EventBus bus;
    agent::TaskService tasks(bus, roomy_limits());
    std::set<std::string> ids;
    for (int i = 0; i < 10000; ++i)
        ids.insert(tasks.create(make_record(agent::TaskKind::Command, "w", "x")));
    ASSERT_EQ(ids.size(), 10000u);
}

TEST(task_service_no_callback_under_lock) {
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    const std::string id = tasks.create(make_record(agent::TaskKind::Command, "w", "x"));
    ASSERT_TRUE(tasks.started(id, 1));
    // Re-enter the service from the backend callback: the map lock must not be
    // held while cancel() invokes it, or this deadlocks.
    ASSERT_TRUE(tasks.attach_cancel(
        id, [&](const agent::CancellationToken&) { return tasks.all().size() == 1u; }));
    agent::CancellationToken token;
    ASSERT_TRUE(tasks.cancel(id, "reentrant", token));
    ASSERT_EQ(tasks.get(id)->state, agent::TaskState::Cancelled);
}

TEST(task_service_bounds_record_fields) {
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    agent::TaskRecord record = make_record(agent::TaskKind::Command, "w", std::string(4000, 'x'));
    record.result.summary = std::string(10000, 's');
    record.result.output_tail = std::string(100000, 'o');
    const std::string id = tasks.create(std::move(record));
    auto stored = tasks.get(id);
    ASSERT_TRUE(stored.has_value());
    ASSERT_TRUE(stored->label.size() <= 128u);
    ASSERT_TRUE(stored->result.summary.size() <= 2048u);
    ASSERT_TRUE(stored->result.output_tail.size() <= 8192u);
}

TEST(task_service_wait_returns_a_terminal_record) {
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    const std::string id = tasks.create(make_record(agent::TaskKind::Command, "w", "x"));
    ASSERT_TRUE(tasks.started(id, 1));
    std::thread finish([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        tasks.finish(id, agent::TaskState::Succeeded, {});
    });
    agent::CancellationToken token;
    const auto record = tasks.wait_for_terminal(id, 1000, token);
    finish.join();
    ASSERT_TRUE(record.has_value());
    ASSERT_EQ(record->state, agent::TaskState::Succeeded);
}

TEST(task_service_wait_times_out_without_a_transition) {
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    const std::string id = tasks.create(make_record(agent::TaskKind::Command, "w", "x"));
    ASSERT_TRUE(tasks.started(id, 1));
    agent::CancellationToken token;
    ASSERT_FALSE(tasks.wait_for_terminal(id, 20, token).has_value());
    ASSERT_TRUE(tasks.finish(id, agent::TaskState::Succeeded, {}));
}

TEST(task_service_adopt_job_remembers_the_backend) {
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    const std::string id =
        tasks.adopt_job("job-7", make_record(agent::TaskKind::Command, "w", "build"));
    ASSERT_FALSE(id.empty());
    ASSERT_EQ(tasks.backend_id(id), std::string("job-7"));
    ASSERT_EQ(tasks.backend_id("missing"), std::string());
}

TEST(job_completion_sink_fires_once_on_exit) {
    agent::JobService jobs;
    std::atomic<int> calls{0};
    std::atomic<int> code{-1};
    const std::string id =
        jobs.start("sh -c 'exit 3'", "", 5, 5, {}, [&](const agent::JobInfo& info) {
            ++calls;
            code = info.exit_code;
        });
    ASSERT_FALSE(id.empty());
    for (int i = 0; i < 500 && calls.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT_EQ(calls.load(), 1);
    ASSERT_EQ(code.load(), 3);
}

// The durability boundary: a completion is on disk before its event reaches a
// subscriber, so a crash between the two can lose the message but never the
// outcome.
TEST(task_service_saves_completion_before_publishing) {
    agent::test::XdgGuard xdg("task_service_persist_order");
    agent::TaskStore store(agent::Workspace::state_dir(), agent::cache_dir());
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    tasks.attach_store(&store);

    bool on_disk = false;
    auto subscription = agent::Events(bus).subscribe<agent::TaskCompletedEvent>(
        [&](const agent::TaskCompletedEvent& ev) {
            const auto record = store.load(ev.id);
            on_disk = record.has_value() && record->state == agent::TaskState::Succeeded &&
                      record->result.exit_code == 0;
        });

    const std::string id = tasks.create(make_record(agent::TaskKind::Command, "w", "build"));
    ASSERT_TRUE(tasks.started(id, 1));
    agent::TaskResult result;
    result.exit_code = 0;
    result.summary = "built";
    ASSERT_TRUE(tasks.finish(id, agent::TaskState::Succeeded, result));
    ASSERT_TRUE(on_disk);
}

TEST(task_service_persists_each_transition) {
    agent::test::XdgGuard xdg("task_service_persist_transitions");
    agent::TaskStore store(agent::Workspace::state_dir(), agent::cache_dir());
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    tasks.attach_store(&store);

    const std::string id = tasks.create(make_record(agent::TaskKind::Command, "w", "build"));
    auto record = store.load(id);
    ASSERT_TRUE(record.has_value());
    ASSERT_EQ(record->state, agent::TaskState::Queued);

    ASSERT_TRUE(tasks.started(id, 1000));
    record = store.load(id);
    ASSERT_TRUE(record.has_value());
    ASSERT_EQ(record->state, agent::TaskState::Running);
    ASSERT_EQ(record->started_ms, 1000);

    ASSERT_TRUE(tasks.progress(id, "step 4"));
    record = store.load(id);
    ASSERT_TRUE(record.has_value());
    ASSERT_EQ(record->progress, std::string("step 4"));
    ASSERT_TRUE(record->last_activity_ms >= 1000);

    agent::TaskResult result;
    result.exit_code = 0;
    result.summary = "done";
    ASSERT_TRUE(tasks.finish(id, agent::TaskState::Succeeded, result));
    record = store.load(id);
    ASSERT_TRUE(record.has_value());
    ASSERT_EQ(record->state, agent::TaskState::Succeeded);
    ASSERT_EQ(record->result.summary, std::string("done"));
    ASSERT_EQ(record->delivered_at_ms, 0); // the inbox still owes the message
}

// A one-shot host waits on the inventory, not on a watch-specific list: the
// completion event wakes the join and the record is returned exactly once.
TEST(task_join_waits_for_a_completion_event) {
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    const std::string id = tasks.create(make_record(agent::TaskKind::Command, "w", "build"));
    ASSERT_TRUE(tasks.started(id, 1));
    std::thread finisher([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        tasks.finish(id, agent::TaskState::Succeeded, {});
    });
    agent::CancellationToken token;
    const auto joined =
        agent::join_tasks(tasks, bus, "w", {std::chrono::milliseconds(2000), false}, token);
    finisher.join();
    ASSERT_EQ(joined.completed.size(), 1u);
    ASSERT_EQ(joined.completed[0].id, id);
    ASSERT_TRUE(joined.still_running.empty());
}

TEST(task_join_deadline_reports_what_remains) {
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    const std::string id = tasks.create(make_record(agent::TaskKind::Command, "w", "build"));
    ASSERT_TRUE(tasks.started(id, 1));
    agent::CancellationToken token;
    const auto started = std::chrono::steady_clock::now();
    const auto joined =
        agent::join_tasks(tasks, bus, "w", {std::chrono::milliseconds(80), false}, token);
    const auto waited = std::chrono::steady_clock::now() - started;
    ASSERT_EQ(joined.still_running.size(), 1u);
    ASSERT_EQ(joined.still_running[0], id);
    ASSERT(waited < std::chrono::seconds(5));
    ASSERT_TRUE(tasks.finish(id, agent::TaskState::Succeeded, {}));
}

TEST(task_join_zero_deadline_samples_without_waiting) {
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    const std::string id = tasks.create(make_record(agent::TaskKind::Command, "w", "build"));
    ASSERT_TRUE(tasks.started(id, 1));
    agent::CancellationToken token;
    const auto started = std::chrono::steady_clock::now();
    const auto joined =
        agent::join_tasks(tasks, bus, "w", {std::chrono::milliseconds(0), false}, token);
    const auto waited = std::chrono::steady_clock::now() - started;
    ASSERT_EQ(joined.still_running.size(), 1u);
    ASSERT(waited < std::chrono::milliseconds(100));
    ASSERT_TRUE(tasks.finish(id, agent::TaskState::Succeeded, {}));
}

TEST(task_join_cancellation_ends_the_wait) {
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    const std::string id = tasks.create(make_record(agent::TaskKind::Command, "w", "build"));
    ASSERT_TRUE(tasks.started(id, 1));
    agent::CancellationToken token;
    std::thread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        token.request();
    });
    const auto started = std::chrono::steady_clock::now();
    const auto joined =
        agent::join_tasks(tasks, bus, "w", {std::chrono::milliseconds(5000), false}, token);
    canceller.join();
    const auto waited = std::chrono::steady_clock::now() - started;
    ASSERT_EQ(joined.still_running.size(), 1u);
    ASSERT(waited < std::chrono::seconds(2));
    ASSERT_TRUE(tasks.finish(id, agent::TaskState::Succeeded, {}));
}
