#include "agent/config.h"
#include "agent/config_paths.h"
#include "agent/events.h"
#include "agent/task.h"
#include "agent/task_inbox.h"
#include "agent/task_store.h"
#include "agent/workspace.h"
#include "tests/test_util.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>

// The failure message prints the record's enums by name.
namespace agent {
inline std::ostream& operator<<(std::ostream& os, TaskState state) {
    return os << task_state_name(state);
}
inline std::ostream& operator<<(std::ostream& os, TaskKind kind) {
    return os << task_kind_name(kind);
}
inline std::ostream& operator<<(std::ostream& os, TaskNotify notify) {
    switch (notify) {
    case TaskNotify::Auto:
        return os << "auto";
    case TaskNotify::Digest:
        return os << "digest";
    case TaskNotify::Silent:
        return os << "silent";
    }
    return os << "auto";
}
} // namespace agent

namespace {

namespace fs = std::filesystem;

// The real layout authority, under a scratch XDG tree: Workspace::state_dir()
// decides the project slug and cache_dir() the cache root, exactly as the host
// does. The XdgGuard restores the developer's environment afterwards.
struct StoreFixture {
    agent::test::XdgGuard xdg;
    agent::TaskStore store;

    explicit StoreFixture(const std::string& name)
        : xdg(name), store(agent::Workspace::state_dir(), agent::cache_dir()) {}
};

agent::TaskRecord record(const std::string& id, const std::string& owner, std::int64_t created,
                         agent::TaskState state) {
    agent::TaskRecord r;
    r.id = id;
    r.kind = agent::TaskKind::Command;
    r.state = state;
    r.owner.owner_id = owner;
    r.owner.project_root = "/project";
    r.label = "job " + id;
    r.created_ms = created;
    r.last_activity_ms = created;
    if (agent::task_state_terminal(state))
        r.ended_ms = created + 5;
    return r;
}

agent::TaskRecord delivered_terminal(const std::string& id, const std::string& owner,
                                     std::int64_t created) {
    agent::TaskRecord r = record(id, owner, created, agent::TaskState::Succeeded);
    r.delivered_at_ms = created + 6;
    return r;
}

std::int64_t wall_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

mode_t mode_of(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0)
        return 0;
    return st.st_mode & 0777;
}

} // namespace

TEST(task_store_round_trips_record) {
    StoreFixture f("task_store_round_trip");
    agent::TaskRecord r;
    r.id = "task_round";
    r.kind = agent::TaskKind::Watch;
    r.state = agent::TaskState::Failed;
    r.notify = agent::TaskNotify::Digest;
    r.label = "watch the deploy";
    r.owner.owner_id = "w1";
    r.owner.project_root = "/work/project";
    r.owner.worktree = "/work/project/.wt/deploy";
    r.command = "make deploy";
    r.goal = "confirm the rollout";
    r.cwd = "/work/project";
    r.created_ms = 100;
    r.started_ms = 110;
    r.last_activity_ms = 120;
    r.ended_ms = 130;
    r.progress = "step 4 of 6";
    r.result.exit_code = 3;
    r.result.signal = 0;
    r.result.summary = "deploy failed";
    r.result.output_tail = "last line";
    r.result.artifact_path = "/cache/task_round/output.txt";
    r.error = "exit 3";
    r.cancel_reason = "not cancelled";
    r.delivered_at_ms = 140;

    ASSERT_TRUE(f.store.save(r));
    const auto loaded = f.store.load("task_round");
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->id, r.id);
    ASSERT_EQ(loaded->kind, r.kind);
    ASSERT_EQ(loaded->state, r.state);
    ASSERT_EQ(loaded->notify, r.notify);
    ASSERT_EQ(loaded->label, r.label);
    ASSERT_EQ(loaded->owner.owner_id, r.owner.owner_id);
    ASSERT_EQ(loaded->owner.project_root, r.owner.project_root);
    ASSERT_EQ(loaded->owner.worktree, r.owner.worktree);
    ASSERT_EQ(loaded->command, r.command);
    ASSERT_EQ(loaded->goal, r.goal);
    ASSERT_EQ(loaded->cwd, r.cwd);
    ASSERT_EQ(loaded->created_ms, r.created_ms);
    ASSERT_EQ(loaded->started_ms, r.started_ms);
    ASSERT_EQ(loaded->last_activity_ms, r.last_activity_ms);
    ASSERT_EQ(loaded->ended_ms, r.ended_ms);
    ASSERT_EQ(loaded->progress, r.progress);
    ASSERT_EQ(loaded->result.exit_code, r.result.exit_code);
    ASSERT_EQ(loaded->result.summary, r.result.summary);
    ASSERT_EQ(loaded->result.output_tail, r.result.output_tail);
    ASSERT_EQ(loaded->result.artifact_path, r.result.artifact_path);
    ASSERT_EQ(loaded->error, r.error);
    ASSERT_EQ(loaded->cancel_reason, r.cancel_reason);
    ASSERT_EQ(loaded->delivered_at_ms, r.delivered_at_ms);

    ASSERT_EQ(f.store.list("w1").size(), 1u);
    ASSERT_EQ(f.store.list("other").size(), 0u);
    ASSERT_EQ(f.store.list().size(), 1u);
    // The store resolves through the same layout authority the readout prints.
    ASSERT_EQ(f.store.records_dir(), agent::task_records_dir(agent::Workspace::state_dir()));
    ASSERT_EQ(f.store.artifacts_root(), agent::task_artifacts_dir(agent::cache_dir()));
}

TEST(task_store_save_is_atomic) {
    StoreFixture f("task_store_atomic");
    agent::TaskRecord first = record("task_atomic", "w", 10, agent::TaskState::Succeeded);
    first.label = "the durable one";
    ASSERT_TRUE(f.store.save(first));

    // A crash during the next write leaves a temporary file behind; the real
    // record is only ever replaced by the final rename.
    {
        std::ofstream partial(f.store.records_dir() + "/task_atomic.json.tmp-crash");
        partial << "{\"schema\":1,\"id\":";
    }

    const auto loaded = f.store.load("task_atomic");
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->label, std::string("the durable one"));
    ASSERT_EQ(f.store.list("w").size(), 1u);
    ASSERT_EQ(f.store.skipped(), 0u);
}

TEST(task_store_marks_delivered_once) {
    StoreFixture f("task_store_delivered");
    ASSERT_TRUE(f.store.save(record("task_once", "w", 10, agent::TaskState::Succeeded)));

    ASSERT_EQ(f.store.undelivered("w").size(), 1u);
    ASSERT_TRUE(f.store.mark_delivered("task_once", 500));
    const auto loaded = f.store.load("task_once");
    ASSERT_TRUE(loaded.has_value());
    ASSERT_EQ(loaded->delivered_at_ms, 500);
    ASSERT_TRUE(f.store.undelivered("w").empty());
    ASSERT_FALSE(f.store.mark_delivered("task_once", 900));
    ASSERT_FALSE(f.store.mark_delivered("missing", 900));
}

TEST(task_store_undelivered_after_restart) {
    StoreFixture f("task_store_undelivered");
    ASSERT_TRUE(f.store.save(record("task_done", "w", 10, agent::TaskState::Succeeded)));

    agent::EventBus bus;
    agent::TaskService first(bus);
    first.attach_store(&f.store);
    ASSERT_EQ(first.restore_from_store(), 1u);
    ASSERT_EQ(first.undelivered().size(), 1u);

    agent::TaskInbox inbox(bus, first);
    ASSERT_EQ(inbox.seed_undelivered(), 1u);
    const auto digest = inbox.take("w");
    ASSERT_EQ(digest.size(), 1u);
    inbox.ack("w", {"task_done"});

    const auto acked = f.store.load("task_done");
    ASSERT_TRUE(acked.has_value());
    ASSERT_TRUE(acked->delivered_at_ms != 0);

    // The next process restores the same record but must not deliver it again.
    agent::TaskService second(bus);
    second.attach_store(&f.store);
    ASSERT_EQ(second.restore_from_store(), 1u);
    agent::TaskInbox inbox2(bus, second);
    ASSERT_EQ(inbox2.seed_undelivered(), 0u);
    ASSERT_TRUE(inbox2.take("w").empty());
}

TEST(task_store_running_becomes_unknown_on_crash) {
    StoreFixture f("task_store_crash");
    agent::TaskRecord running = record("task_crash", "w", 100, agent::TaskState::Running);
    running.last_activity_ms = 900;
    running.progress = "step 4";
    running.started_ms = 150;
    ASSERT_TRUE(f.store.save(running));

    agent::EventBus bus;
    agent::TaskService tasks(bus);
    tasks.attach_store(&f.store);
    ASSERT_EQ(tasks.restore_from_store(), 1u);

    const auto restored = tasks.get("task_crash");
    ASSERT_TRUE(restored.has_value());
    ASSERT_EQ(restored->state, agent::TaskState::Unknown);
    ASSERT_EQ(restored->progress, std::string("step 4"));
    ASSERT_EQ(restored->ended_ms, 900);
    ASSERT_TRUE(restored->error.find("host exited") != std::string::npos);

    // The restart rule is durable, not a display-only rewrite.
    const auto disk = f.store.load("task_crash");
    ASSERT_TRUE(disk.has_value());
    ASSERT_EQ(disk->state, agent::TaskState::Unknown);
    ASSERT_EQ(disk->ended_ms, 900);
}

TEST(task_store_running_becomes_interrupted_on_clean_shutdown) {
    StoreFixture f("task_store_clean_shutdown");
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    tasks.attach_store(&f.store);

    const std::string id = tasks.create(record("", "w", 100, agent::TaskState::Queued));
    ASSERT_FALSE(id.empty());
    ASSERT_TRUE(tasks.started(id, 1000));
    ASSERT_TRUE(tasks.progress(id, "half way"));
    tasks.shutdown();

    const auto stopped = tasks.get(id);
    ASSERT_TRUE(stopped.has_value());
    ASSERT_EQ(stopped->state, agent::TaskState::Interrupted);
    ASSERT_TRUE(stopped->error.find("host shut down") != std::string::npos);

    agent::TaskService restarted(bus);
    restarted.attach_store(&f.store);
    ASSERT_EQ(restarted.restore_from_store(), 1u);
    const auto back = restarted.get(id);
    ASSERT_TRUE(back.has_value());
    ASSERT_EQ(back->state, agent::TaskState::Interrupted);
}

TEST(task_store_queued_becomes_cancelled_on_restart) {
    StoreFixture f("task_store_queued");
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    tasks.attach_store(&f.store);

    const std::string id = tasks.create(record("", "w", 100, agent::TaskState::Queued));
    ASSERT_FALSE(id.empty());
    // No started(): the process dies with the record still Queued.

    agent::TaskService restarted(bus);
    restarted.attach_store(&f.store);
    ASSERT_EQ(restarted.restore_from_store(), 1u);
    const auto back = restarted.get(id);
    ASSERT_TRUE(back.has_value());
    ASSERT_EQ(back->state, agent::TaskState::Cancelled);
    ASSERT_EQ(back->cancel_reason, std::string("host restarted before start"));

    const auto disk = f.store.load(id);
    ASSERT_TRUE(disk.has_value());
    ASSERT_EQ(disk->state, agent::TaskState::Cancelled);
}

TEST(task_store_prunes_to_200_records) {
    StoreFixture f("task_store_200");
    for (int i = 1; i <= 205; ++i) {
        const std::string id = "task_" + std::to_string(1000 + i);
        ASSERT_TRUE(f.store.save(delivered_terminal(id, "w", i)));
    }

    const agent::TaskPruneReport report = f.store.prune(wall_now_ms());
    ASSERT_EQ(report.records_removed, 5u);
    ASSERT_EQ(f.store.list("w").size(), agent::TaskStore::kRecordLimit);
    for (int i = 1; i <= 5; ++i)
        ASSERT_FALSE(f.store.load("task_" + std::to_string(1000 + i)).has_value());
    ASSERT_TRUE(f.store.load("task_1205").has_value());
}

TEST(task_store_prunes_artifacts_by_age_and_size) {
    StoreFixture f("task_store_artifacts");
    ASSERT_TRUE(f.store.save(delivered_terminal("task_old", "w", 10)));
    ASSERT_TRUE(f.store.save(delivered_terminal("task_big", "w", 20)));

    const std::string old_dir = f.store.artifact_dir("task_old");
    ASSERT_FALSE(old_dir.empty());
    const std::string old_file = old_dir + "/output.txt";
    {
        std::ofstream out(old_file);
        out << "old output";
    }
    fs::last_write_time(old_file, fs::file_time_type::clock::now() - std::chrono::hours(24 * 8));

    const std::string big_dir = f.store.artifact_dir("task_big");
    ASSERT_FALSE(big_dir.empty());
    const std::string big_file = big_dir + "/transcript.txt";
    {
        std::ofstream out(big_file);
        out << "x";
    }
    fs::resize_file(big_file, agent::TaskStore::kArtifactBytes + 1024 * 1024);

    const agent::TaskPruneReport report = f.store.prune(wall_now_ms());
    ASSERT_TRUE(report.artifacts_removed >= 2u);
    ASSERT_TRUE(report.bytes_removed >= agent::TaskStore::kArtifactBytes);
    ASSERT_FALSE(fs::exists(old_file));
    ASSERT_FALSE(fs::exists(big_file));
}

TEST(task_store_never_prunes_undelivered) {
    StoreFixture f("task_store_protect");
    agent::TaskRecord keep = record("task_keep", "w", 10, agent::TaskState::Succeeded);
    keep.delivered_at_ms = 0;
    ASSERT_TRUE(f.store.save(keep));

    const std::string dir = f.store.artifact_dir("task_keep");
    ASSERT_FALSE(dir.empty());
    const std::string file = dir + "/output.txt";
    {
        std::ofstream out(file);
        out << "still owed to its owner";
    }
    fs::last_write_time(file, fs::file_time_type::clock::now() - std::chrono::hours(24 * 30));

    const agent::TaskPruneReport report = f.store.prune(wall_now_ms());
    ASSERT_EQ(report.artifacts_removed, 0u);
    ASSERT_TRUE(fs::exists(file));
    ASSERT_TRUE(f.store.load("task_keep").has_value());
}

TEST(task_store_skips_malformed_record) {
    StoreFixture f("task_store_malformed");
    ASSERT_TRUE(f.store.save(delivered_terminal("task_good", "w", 10)));

    {
        std::ofstream bad(f.store.records_dir() + "/broken.json");
        bad << "{ this is not json";
    }
    {
        std::ofstream newer(f.store.records_dir() + "/future.json");
        newer << R"({"schema":99,"id":"future"})";
    }

    const auto records = f.store.list("w");
    ASSERT_EQ(records.size(), 1u);
    ASSERT_EQ(records[0].id, std::string("task_good"));
    ASSERT_EQ(f.store.skipped(), 2u);
}

TEST(task_store_modes_are_private) {
    const mode_t previous = ::umask(022);
    {
        StoreFixture f("task_store_modes");
        ASSERT_TRUE(f.store.save(record("task_mode", "w", 10, agent::TaskState::Queued)));
        ASSERT_FALSE(f.store.artifact_dir("task_mode").empty());

        ASSERT_EQ(mode_of(f.store.records_dir()), static_cast<mode_t>(0700));
        ASSERT_EQ(mode_of(f.store.records_dir() + "/task_mode.json"), static_cast<mode_t>(0600));
        ASSERT_EQ(mode_of(f.store.artifacts_root() + "/task_mode"), static_cast<mode_t>(0700));
    }
    ::umask(previous);
}

// A queued task never started, so a clean exit cancels it rather than leaving a
// zombie the next process would have to explain.
TEST(task_store_clean_shutdown_cancels_queued) {
    StoreFixture f("task_store_shutdown_queued");
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    tasks.attach_store(&f.store);

    const std::string id = tasks.create(record("", "w", 100, agent::TaskState::Queued));
    ASSERT_FALSE(id.empty());
    tasks.shutdown();

    const auto record_now = tasks.get(id);
    ASSERT_TRUE(record_now.has_value());
    ASSERT_EQ(record_now->state, agent::TaskState::Cancelled);
    ASSERT_EQ(record_now->cancel_reason, std::string("host shut down before start"));
    const auto disk = f.store.load(id);
    ASSERT_TRUE(disk.has_value());
    ASSERT_EQ(disk->state, agent::TaskState::Cancelled);
}

// A silent task owes no message: it is marked delivered when it finishes, so it
// is neither seeded into the inbox nor kept alive by the retention window.
TEST(task_store_silent_tasks_do_not_wait_for_delivery) {
    StoreFixture f("task_store_silent");
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    tasks.attach_store(&f.store);

    agent::TaskRecord silent = record("", "w", 100, agent::TaskState::Queued);
    silent.notify = agent::TaskNotify::Silent;
    const std::string id = tasks.create(silent);
    ASSERT_FALSE(id.empty());
    ASSERT_TRUE(tasks.started(id, 110));
    ASSERT_TRUE(tasks.finish(id, agent::TaskState::Succeeded, {}));

    const auto disk = f.store.load(id);
    ASSERT_TRUE(disk.has_value());
    ASSERT_TRUE(disk->delivered_at_ms != 0);
    ASSERT_TRUE(tasks.undelivered().empty());

    agent::TaskInbox inbox(bus, tasks);
    ASSERT_EQ(inbox.seed_undelivered(), 0u);
}

// A live record always wins over the one on disk: restoring must not resurrect
// an older state under an id the process is already using.
TEST(task_store_live_record_wins_over_disk) {
    StoreFixture f("task_store_live");
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    tasks.attach_store(&f.store);

    const std::string id = tasks.create(record("task_live", "w", 100, agent::TaskState::Queued));
    ASSERT_EQ(id, std::string("task_live"));
    ASSERT_TRUE(f.store.save(delivered_terminal("task_live", "w", 100)));

    ASSERT_EQ(tasks.restore_from_store(), 0u);
    const auto live = tasks.get("task_live");
    ASSERT_TRUE(live.has_value());
    ASSERT_EQ(live->state, agent::TaskState::Queued);
}
