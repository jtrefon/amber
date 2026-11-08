#ifndef AGENT_TASK_STORE_H
#define AGENT_TASK_STORE_H

#include "agent/task.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace agent {

// What one retention pass did. Errors are counted, never fatal: a store that
// cannot remove one old file must not stop the host from starting.
struct TaskPruneReport {
    std::size_t records_removed = 0;
    std::size_t artifacts_removed = 0;
    std::uintmax_t bytes_removed = 0;
    std::size_t errors = 0;
};

// The durable half of the task inventory: one private JSON record per task
// under <state>/tasks, and per-task artifact directories under <cache>/tasks.
//
// Records are written atomically (temp file, fsync, rename) and are the only
// durable promise. A process is never reattached; a record whose owner died is
// turned into Unknown by TaskService on the next start. There is no single
// index that can half-write: list() scans the directory.
//
// The store is safe to call from any thread. TaskService nevertheless keeps its
// own persistence lock so the newest snapshot is the one left on disk.
class TaskStore {
public:
    TaskStore(const std::string& state_dir, const std::string& cache_dir);

    TaskStore(const TaskStore&) = delete;
    TaskStore& operator=(const TaskStore&) = delete;

    // Atomic and private. False on any I/O error; the caller reports it and
    // keeps running.
    bool save(const TaskRecord& record);
    std::optional<TaskRecord> load(const std::string& id) const;

    // Newest first, bounded to the retention window. `owner_id` empty lists
    // every owner. Malformed records are skipped and counted in skipped().
    std::vector<TaskRecord> list(const std::string& owner_id = {}) const;
    // Terminal records not yet accepted by their owner; the inbox seeds these
    // on startup and acks them through TaskService.
    std::vector<TaskRecord> undelivered(const std::string& owner_id = {}) const;
    bool mark_delivered(const std::string& id, std::int64_t now_ms);

    // Creates <cache>/tasks/<id> with 0700 and returns it.
    std::string artifact_dir(const std::string& id) const;

    // Keep the newest records per owner, prune artifact files older than seven
    // days or beyond the total byte budget, and never remove an undelivered
    // record or a non-terminal task's artifact.
    TaskPruneReport prune(std::int64_t now_ms);
    TaskPruneReport last_prune() const noexcept;

    // Records skipped by the last list()/undelivered() scan (malformed,
    // unreadable, or a schema this build does not know).
    std::size_t skipped() const noexcept;

    const std::string& records_dir() const noexcept { return records_dir_; }
    const std::string& artifacts_root() const noexcept { return artifacts_root_; }

    static constexpr std::size_t kRecordLimit = 200;
    static constexpr std::uintmax_t kArtifactBytes = 256ull * 1024ull * 1024ull;
    static constexpr int kArtifactDays = 7;

private:
    std::string record_path(const std::string& id) const;

    // Caller holds mtx_.
    std::optional<TaskRecord> load_unlocked(const std::string& id) const;
    bool save_unlocked(const TaskRecord& record);
    std::vector<TaskRecord> scan_unlocked(const std::string& owner_id, std::size_t limit) const;
    TaskPruneReport prune_records_unlocked(const std::vector<TaskRecord>& records);
    TaskPruneReport prune_artifacts_unlocked(const std::vector<TaskRecord>& records,
                                             std::int64_t now_ms);

    std::string records_dir_;
    std::string artifacts_root_;
    mutable std::mutex mtx_;
    mutable std::size_t skipped_ = 0;
    TaskPruneReport last_prune_;
};

} // namespace agent

#endif // AGENT_TASK_STORE_H
