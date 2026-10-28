#include "watch_store.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace agent::plugins {

namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string join_path(const std::string& a, const std::string& b) {
    if (a.empty())
        return b;
    return (fs::path(a) / b).string();
}

bool write_text(const std::string& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return out.good();
}

json record_json(const WatchRecord& r) {
    return json{{"id", r.id},
                {"owner_id", r.owner_id},
                {"label", r.label},
                {"profile", r.profile},
                {"command", r.command},
                {"pr", r.pr},
                {"url", r.url},
                {"cwd", r.cwd},
                {"state", watch_state_name(r.state)},
                {"created_ms", r.created_ms},
                {"seq", r.seq},
                {"started_ms", r.started_ms},
                {"ended_ms", r.ended_ms},
                {"interval_s", r.interval_s},
                {"deadline_s", r.deadline_s},
                {"attempt", r.attempt},
                {"success_exit", r.success_exit},
                {"observer", r.observer},
                {"delivered", r.delivered},
                {"abandoned", r.abandoned},
                {"head_sha", r.head_sha},
                {"previous_summary", r.previous_summary},
                {"last_status", r.last_status},
                {"journal", r.journal},
                {"report_id", r.report_id},
                {"artifact_dir", r.artifact_dir}};
}

bool read_string(const json& j, const char* key, std::string& out) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string())
        return false;
    out = it->get<std::string>();
    return true;
}

WatchRecord record_from_json(const json& j) {
    WatchRecord r;
    read_string(j, "id", r.id);
    read_string(j, "owner_id", r.owner_id);
    read_string(j, "label", r.label);
    read_string(j, "profile", r.profile);
    read_string(j, "command", r.command);
    read_string(j, "pr", r.pr);
    read_string(j, "url", r.url);
    read_string(j, "cwd", r.cwd);
    std::string state;
    if (read_string(j, "state", state))
        r.state = watch_state_from(state);
    r.created_ms = j.value("created_ms", 0L);
    r.seq = j.value("seq", 0L);
    r.started_ms = j.value("started_ms", 0L);
    r.ended_ms = j.value("ended_ms", 0L);
    r.interval_s = j.value("interval_s", 20);
    r.deadline_s = j.value("deadline_s", 1800);
    r.attempt = j.value("attempt", 1);
    r.success_exit = j.value("success_exit", 0);
    r.observer = j.value("observer", true);
    r.delivered = j.value("delivered", false);
    r.abandoned = j.value("abandoned", false);
    read_string(j, "head_sha", r.head_sha);
    read_string(j, "previous_summary", r.previous_summary);
    read_string(j, "last_status", r.last_status);
    if (j.contains("journal") && j["journal"].is_array())
        for (const auto& item : j["journal"])
            if (item.is_string())
                r.journal.push_back(item.get<std::string>());
    read_string(j, "report_id", r.report_id);
    read_string(j, "artifact_dir", r.artifact_dir);
    return r;
}

json report_json(const WatchReport& r) {
    return json{{"watch_id", r.watch_id},
                {"report_id", r.report_id},
                {"state", watch_state_name(r.state)},
                {"headline", r.headline},
                {"observed", r.observed},
                {"likely_cause", r.likely_cause},
                {"next_action", r.next_action},
                {"confidence", r.confidence},
                {"needs_attention", r.needs_attention},
                {"journal_update", r.journal_update},
                {"artifact_path", r.artifact_path},
                {"format_error", r.format_error},
                {"attempt", r.attempt}};
}

WatchReport report_from_json(const json& j) {
    WatchReport r;
    read_string(j, "watch_id", r.watch_id);
    read_string(j, "report_id", r.report_id);
    std::string state;
    if (read_string(j, "state", state))
        r.state = watch_state_from(state);
    read_string(j, "headline", r.headline);
    if (j.contains("observed") && j["observed"].is_array())
        for (const auto& item : j["observed"])
            if (item.is_string())
                r.observed.push_back(item.get<std::string>());
    read_string(j, "likely_cause", r.likely_cause);
    read_string(j, "next_action", r.next_action);
    read_string(j, "confidence", r.confidence);
    r.needs_attention = j.value("needs_attention", false);
    read_string(j, "journal_update", r.journal_update);
    read_string(j, "artifact_path", r.artifact_path);
    r.format_error = j.value("format_error", false);
    r.attempt = j.value("attempt", 1);
    return r;
}

} // namespace

WatchStore::WatchStore(std::string root) : root_(std::move(root)) {
    if (root_.empty())
        root_ = "watch";
    // Intentionally no mkdir here: constructing the plugin (and therefore the
    // store) is not a watch, and a hermetic benchmark run must leave no state
    // behind. The first save/artifact creates its directory.
}

std::string WatchStore::records_dir() const {
    return join_path(root_, "records");
}

std::string WatchStore::reports_dir() const {
    return join_path(root_, "reports");
}

std::string WatchStore::artifacts_dir() const {
    return join_path(root_, "artifacts");
}

std::vector<WatchRecord> WatchStore::load_records() const {
    std::vector<WatchRecord> out;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(records_dir(), ec)) {
        if (ec || !entry.is_regular_file() || entry.path().extension() != ".json")
            continue;
        std::ifstream in(entry.path());
        if (!in)
            continue;
        try {
            json j;
            in >> j;
            out.push_back(record_from_json(j));
        } catch (const std::exception&) {
            // A corrupt record is skipped; the next save rewrites the file.
        }
    }
    std::sort(out.begin(), out.end(), [](const WatchRecord& a, const WatchRecord& b) {
        return a.created_ms < b.created_ms;
    });
    return out;
}

bool WatchStore::save_record(const WatchRecord& record) const {
    if (record.id.empty())
        return false;
    return write_text(join_path(records_dir(), record.id + ".json"), record_json(record).dump(2));
}

bool WatchStore::save_report(const WatchReport& report) const {
    if (report.watch_id.empty())
        return false;
    return write_text(join_path(reports_dir(), report.watch_id + ".json"),
                      report_json(report).dump(2));
}

bool WatchStore::load_report(const std::string& watch_id, WatchReport& out) const {
    std::ifstream in(join_path(reports_dir(), watch_id + ".json"));
    if (!in)
        return false;
    try {
        json j;
        in >> j;
        out = report_from_json(j);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool WatchStore::remove(const std::string& watch_id) const {
    if (watch_id.empty())
        return false;
    std::error_code ec;
    bool removed = fs::remove(join_path(records_dir(), watch_id + ".json"), ec);
    removed = fs::remove(join_path(reports_dir(), watch_id + ".json"), ec) || removed;
    removed = fs::remove_all(join_path(artifacts_dir(), watch_id), ec) > 0 || removed;
    return removed;
}

std::string WatchStore::artifact_dir(const std::string& watch_id) const {
    const std::string dir = join_path(artifacts_dir(), watch_id);
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

std::string WatchStore::write_artifact(const std::string& watch_id, const std::string& name,
                                       const std::string& content, std::size_t cap) const {
    const std::string path = join_path(artifact_dir(watch_id), name);
    std::string body = content;
    if (cap > 0 && body.size() > cap)
        body.resize(cap);
    return write_text(path, body) ? path : std::string();
}

} // namespace agent::plugins
