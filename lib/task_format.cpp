#include "agent/task_format.h"

#include <chrono>
#include <sstream>

namespace agent {

namespace {

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

std::string format_task_duration(std::int64_t started_ms, std::int64_t ended_ms) {
    if (started_ms <= 0)
        return {};
    const std::int64_t end = ended_ms > 0 ? ended_ms : now_ms();
    if (end <= started_ms)
        return "0s";
    const std::int64_t seconds = (end - started_ms) / 1000;
    if (seconds < 60)
        return std::to_string(seconds) + "s";
    if (seconds < 3600)
        return std::to_string(seconds / 60) + "m" + std::to_string(seconds % 60) + "s";
    return std::to_string(seconds / 3600) + "h" + std::to_string((seconds % 3600) / 60) + "m";
}

std::string format_task_row(const TaskRecord& record) {
    std::ostringstream out;
    out << record.id << " | " << task_kind_name(record.kind) << " | "
        << task_state_name(record.state);
    const std::string elapsed = format_task_duration(record.started_ms, record.ended_ms);
    if (!elapsed.empty())
        out << " | " << elapsed;
    if (!record.label.empty())
        out << " | " << record.label;
    if (!record.progress.empty())
        out << " | " << record.progress;
    return out.str();
}

std::string format_task_detail(const TaskRecord& record) {
    std::ostringstream out;
    out << "id: " << record.id << "\n"
        << "kind: " << task_kind_name(record.kind) << "\n"
        << "state: " << task_state_name(record.state) << "\n";
    auto add = [&](const char* name, const std::string& value) {
        if (!value.empty())
            out << name << ": " << value << "\n";
    };
    add("label", record.label);
    add("elapsed", format_task_duration(record.started_ms, record.ended_ms));
    add("owner", record.owner.owner_id);
    add("cwd", record.cwd);
    add("progress", record.progress);
    if (record.state == TaskState::Succeeded || record.state == TaskState::Failed)
        out << "exit: " << record.result.exit_code << "\n";
    if (record.result.signal != 0)
        out << "signal: " << record.result.signal << "\n";
    add("summary", record.result.summary);
    add("error", record.error);
    add("cancel", record.cancel_reason);
    add("artifact", record.result.artifact_path);
    std::string text = out.str();
    if (!text.empty() && text.back() == '\n')
        text.pop_back();
    return text;
}

} // namespace agent
