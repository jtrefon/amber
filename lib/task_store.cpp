#include "agent/task_store.h"

#include "agent/config_paths.h"
#include "agent/private_file.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace agent {

namespace {

namespace fs = std::filesystem;

constexpr int kSchema = 1;
constexpr std::size_t kMaxScan = 4096;

bool valid_id(const std::string& id) {
    if (id.empty() || id.size() > 128 || id == "." || id == "..")
        return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-';
    });
}

bool plain_file(const fs::directory_entry& entry) {
    std::error_code ec;
    const fs::file_status status = entry.symlink_status(ec);
    return !ec && fs::is_regular_file(status) && !fs::is_symlink(status);
}

bool plain_dir(const fs::directory_entry& entry) {
    std::error_code ec;
    const fs::file_status status = entry.symlink_status(ec);
    return !ec && fs::is_directory(status) && !fs::is_symlink(status);
}

bool has_json_suffix(const std::string& name) {
    constexpr std::string_view kSuffix = ".json";
    return name.size() > kSuffix.size() &&
           name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0;
}

TaskState state_from(const std::string& text) {
    if (text == "queued")
        return TaskState::Queued;
    if (text == "running")
        return TaskState::Running;
    if (text == "succeeded")
        return TaskState::Succeeded;
    if (text == "failed")
        return TaskState::Failed;
    if (text == "cancelled")
        return TaskState::Cancelled;
    if (text == "timed_out")
        return TaskState::TimedOut;
    if (text == "interrupted")
        return TaskState::Interrupted;
    return TaskState::Unknown;
}

TaskKind kind_from(const std::string& text) {
    if (text == "watch")
        return TaskKind::Watch;
    if (text == "worker")
        return TaskKind::Worker;
    return TaskKind::Command;
}

TaskNotify notify_from(const std::string& text) {
    if (text == "digest")
        return TaskNotify::Digest;
    if (text == "silent")
        return TaskNotify::Silent;
    return TaskNotify::Auto;
}

const char* notify_name(TaskNotify notify) {
    switch (notify) {
    case TaskNotify::Auto:
        return "auto";
    case TaskNotify::Digest:
        return "digest";
    case TaskNotify::Silent:
        return "silent";
    }
    return "auto";
}

nlohmann::json to_json(const TaskRecord& record) {
    nlohmann::json j;
    j["schema"] = kSchema;
    j["id"] = record.id;
    j["kind"] = task_kind_name(record.kind);
    j["state"] = task_state_name(record.state);
    j["notify"] = notify_name(record.notify);
    j["label"] = record.label;
    j["owner"] = {{"owner_id", record.owner.owner_id},
                  {"project_root", record.owner.project_root},
                  {"worktree", record.owner.worktree}};
    j["command"] = record.command;
    j["goal"] = record.goal;
    j["cwd"] = record.cwd;
    j["created_ms"] = record.created_ms;
    j["started_ms"] = record.started_ms;
    j["last_activity_ms"] = record.last_activity_ms;
    j["ended_ms"] = record.ended_ms;
    j["progress"] = record.progress;
    j["result"] = {{"exit_code", record.result.exit_code},
                   {"signal", record.result.signal},
                   {"summary", record.result.summary},
                   {"output_tail", record.result.output_tail},
                   {"artifact_path", record.result.artifact_path}};
    j["error"] = record.error;
    j["cancel_reason"] = record.cancel_reason;
    j["delivered_at_ms"] = record.delivered_at_ms;
    return j;
}

std::optional<TaskRecord> from_json(const nlohmann::json& j) {
    if (!j.is_object() || !j.contains("id") || !j["id"].is_string())
        return std::nullopt;
    if (j.value("schema", 0) > kSchema)
        return std::nullopt; // a newer build owns this record
    TaskRecord record;
    record.id = j["id"].get<std::string>();
    if (!valid_id(record.id))
        return std::nullopt;
    record.kind = kind_from(j.value("kind", std::string("command")));
    record.state = state_from(j.value("state", std::string("unknown")));
    record.notify = notify_from(j.value("notify", std::string("auto")));
    record.label = j.value("label", std::string());
    if (j.contains("owner") && j["owner"].is_object()) {
        const auto& owner = j["owner"];
        record.owner.owner_id = owner.value("owner_id", std::string());
        record.owner.project_root = owner.value("project_root", std::string());
        record.owner.worktree = owner.value("worktree", std::string());
    }
    record.command = j.value("command", std::string());
    record.goal = j.value("goal", std::string());
    record.cwd = j.value("cwd", std::string());
    record.created_ms = j.value("created_ms", 0LL);
    record.started_ms = j.value("started_ms", 0LL);
    record.last_activity_ms = j.value("last_activity_ms", 0LL);
    record.ended_ms = j.value("ended_ms", 0LL);
    record.progress = j.value("progress", std::string());
    if (j.contains("result") && j["result"].is_object()) {
        const auto& result = j["result"];
        record.result.exit_code = result.value("exit_code", -1);
        record.result.signal = result.value("signal", 0);
        record.result.summary = result.value("summary", std::string());
        record.result.output_tail = result.value("output_tail", std::string());
        record.result.artifact_path = result.value("artifact_path", std::string());
    }
    record.error = j.value("error", std::string());
    record.cancel_reason = j.value("cancel_reason", std::string());
    record.delivered_at_ms = j.value("delivered_at_ms", 0LL);
    return record;
}

bool newer(const TaskRecord& a, const TaskRecord& b) {
    if (a.created_ms != b.created_ms)
        return a.created_ms > b.created_ms;
    return a.id > b.id;
}

std::optional<TaskRecord> read_record_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    try {
        nlohmann::json j;
        in >> j;
        return from_json(j);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// One artifact file and the retention decision it carries.
struct ArtifactFile {
    fs::path path;
    std::uintmax_t size = 0;
    fs::file_time_type mtime;
    bool protected_task = false;
};

bool artifact_is_protected(const std::map<std::string, TaskRecord>& by_id,
                           const std::string& task_id) {
    const auto found = by_id.find(task_id);
    if (found == by_id.end())
        return false;
    return !task_state_terminal(found->second.state) || found->second.delivered_at_ms == 0;
}

void collect_artifact_files(const fs::path& root, const std::map<std::string, TaskRecord>& by_id,
                            std::vector<ArtifactFile>& files) {
    std::error_code ec;
    if (!fs::is_directory(root, ec))
        return;
    for (const auto& dir : fs::directory_iterator(root, ec)) {
        if (ec)
            break;
        if (!plain_dir(dir))
            continue; // a symlink is never walked
        const std::string task_id = dir.path().filename().string();
        const bool protected_task = artifact_is_protected(by_id, task_id);
        std::error_code walk_ec;
        for (const auto& file : fs::directory_iterator(dir.path(), walk_ec)) {
            if (walk_ec)
                break;
            if (!plain_file(file))
                continue;
            std::error_code file_ec;
            ArtifactFile entry;
            entry.path = file.path();
            entry.size = file.file_size(file_ec);
            entry.mtime = fs::last_write_time(file, file_ec);
            entry.protected_task = protected_task;
            if (!file_ec)
                files.push_back(entry);
        }
    }
}

// The caller's clock, expressed on the filesystem clock: a test can advance
// time without sleeping or touching every file.
fs::file_time_type prune_clock(std::int64_t now_ms) {
    fs::file_time_type now = fs::file_time_type::clock::now();
    if (now_ms <= 0)
        return now;
    const auto system_now = std::chrono::system_clock::now();
    const auto fake_now = std::chrono::system_clock::time_point(std::chrono::milliseconds(now_ms));
    const auto skew =
        std::chrono::duration_cast<fs::file_time_type::duration>(system_now - fake_now);
    return now - skew;
}

TaskPruneReport remove_expired(std::vector<ArtifactFile>& files, fs::file_time_type now) {
    TaskPruneReport report;
    std::uintmax_t total = 0;
    for (const auto& entry : files)
        total += entry.size;
    std::sort(files.begin(), files.end(),
              [](const ArtifactFile& a, const ArtifactFile& b) { return a.mtime < b.mtime; });
    const auto max_age = std::chrono::hours(24 * TaskStore::kArtifactDays);
    for (const auto& entry : files) {
        const bool too_old = (now - entry.mtime) > max_age;
        if (entry.protected_task || (!too_old && total <= TaskStore::kArtifactBytes))
            continue;
        std::error_code remove_ec;
        if (fs::remove(entry.path, remove_ec)) {
            total = total > entry.size ? total - entry.size : 0;
            ++report.artifacts_removed;
            report.bytes_removed += entry.size;
        } else if (remove_ec) {
            ++report.errors;
        }
    }
    return report;
}

void remove_empty_artifact_dirs(const fs::path& root) {
    std::error_code ec;
    for (const auto& dir : fs::directory_iterator(root, ec)) {
        if (ec)
            break;
        if (!plain_dir(dir))
            continue;
        std::error_code ignored;
        fs::remove(dir.path(), ignored);
    }
}

} // namespace

TaskStore::TaskStore(const std::string& state_dir, const std::string& cache_dir)
    : records_dir_(task_records_dir(state_dir)), artifacts_root_(task_artifacts_dir(cache_dir)) {}

std::string TaskStore::record_path(const std::string& id) const {
    return records_dir_ + "/" + id + ".json";
}

bool TaskStore::save(const TaskRecord& record) {
    std::scoped_lock lk(mtx_);
    return save_unlocked(record);
}

bool TaskStore::save_unlocked(const TaskRecord& record) {
    if (!valid_id(record.id))
        return false;
    if (!ensure_private_dir(records_dir_))
        return false;
    return write_private_file(record_path(record.id), to_json(record).dump());
}

std::optional<TaskRecord> TaskStore::load(const std::string& id) const {
    std::scoped_lock lk(mtx_);
    return load_unlocked(id);
}

std::optional<TaskRecord> TaskStore::load_unlocked(const std::string& id) const {
    if (!valid_id(id))
        return std::nullopt;
    std::ifstream in(record_path(id), std::ios::binary);
    if (!in)
        return std::nullopt;
    try {
        nlohmann::json j;
        in >> j;
        return from_json(j);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::vector<TaskRecord> TaskStore::scan_unlocked(const std::string& owner_id,
                                                 std::size_t limit) const {
    skipped_ = 0;
    std::vector<TaskRecord> out;
    std::error_code ec;
    if (!fs::is_directory(records_dir_, ec)) {
        if (ec)
            ++skipped_;
        return out;
    }
    std::size_t seen = 0;
    for (const auto& entry : fs::directory_iterator(records_dir_, ec)) {
        if (ec || seen >= kMaxScan)
            break;
        if (!plain_file(entry) || !has_json_suffix(entry.path().filename().string()))
            continue; // a temp file from an interrupted write is not a record
        ++seen;
        auto record = read_record_file(entry.path());
        if (!record) {
            ++skipped_;
            continue;
        }
        if (owner_id.empty() || record->owner.owner_id == owner_id)
            out.push_back(std::move(*record));
    }
    std::sort(out.begin(), out.end(), newer);
    if (out.size() > limit)
        out.resize(limit);
    return out;
}

std::vector<TaskRecord> TaskStore::list(const std::string& owner_id) const {
    std::scoped_lock lk(mtx_);
    return scan_unlocked(owner_id, kRecordLimit);
}

std::vector<TaskRecord> TaskStore::undelivered(const std::string& owner_id) const {
    std::scoped_lock lk(mtx_);
    std::vector<TaskRecord> out;
    for (auto& record : scan_unlocked(owner_id, kMaxScan)) {
        if (record.delivered_at_ms == 0 && record.notify != TaskNotify::Silent &&
            task_state_terminal(record.state))
            out.push_back(std::move(record));
    }
    return out;
}

bool TaskStore::mark_delivered(const std::string& id, std::int64_t now_ms) {
    std::scoped_lock lk(mtx_);
    auto record = load_unlocked(id);
    if (!record || record->delivered_at_ms != 0)
        return false;
    record->delivered_at_ms = now_ms;
    return save_unlocked(*record);
}

std::string TaskStore::artifact_dir(const std::string& id) const {
    if (!valid_id(id))
        return {};
    std::string dir = artifacts_root_ + "/" + id;
    if (!ensure_private_dir(dir))
        return {};
    return dir;
}

TaskPruneReport TaskStore::prune_records_unlocked(const std::vector<TaskRecord>& records) {
    TaskPruneReport report;
    std::map<std::string, std::vector<TaskRecord>> by_owner;
    for (const auto& record : records)
        by_owner[record.owner.owner_id].push_back(record);

    for (auto& [owner, group] : by_owner) {
        (void)owner;
        std::sort(group.begin(), group.end(), newer);
        for (std::size_t i = kRecordLimit; i < group.size(); ++i) {
            if (group[i].delivered_at_ms == 0)
                continue; // never delete an undelivered completion
            std::error_code ec;
            if (fs::remove(record_path(group[i].id), ec))
                ++report.records_removed;
            else if (ec)
                ++report.errors;
        }
    }
    return report;
}

TaskPruneReport TaskStore::prune_artifacts_unlocked(const std::vector<TaskRecord>& records,
                                                    std::int64_t now_ms) {
    std::map<std::string, TaskRecord> by_id;
    for (const auto& record : records)
        by_id[record.id] = record;
    std::vector<ArtifactFile> files;
    collect_artifact_files(artifacts_root_, by_id, files);
    TaskPruneReport report = remove_expired(files, prune_clock(now_ms));
    remove_empty_artifact_dirs(artifacts_root_);
    return report;
}

TaskPruneReport TaskStore::prune(std::int64_t now_ms) {
    std::scoped_lock lk(mtx_);
    const std::vector<TaskRecord> records = scan_unlocked({}, kMaxScan);
    TaskPruneReport report = prune_records_unlocked(records);
    const TaskPruneReport artifacts = prune_artifacts_unlocked(records, now_ms);
    report.artifacts_removed += artifacts.artifacts_removed;
    report.bytes_removed += artifacts.bytes_removed;
    report.errors += artifacts.errors;
    last_prune_ = report;
    return report;
}

TaskPruneReport TaskStore::last_prune() const noexcept {
    std::scoped_lock lk(mtx_);
    return last_prune_;
}

std::size_t TaskStore::skipped() const noexcept {
    std::scoped_lock lk(mtx_);
    return skipped_;
}

} // namespace agent
