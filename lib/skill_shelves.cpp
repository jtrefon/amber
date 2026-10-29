#include "agent/skill_shelves.h"

#include "agent/prompt.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <set>

#include <nlohmann/json.hpp>

namespace agent {

namespace {

namespace fs = std::filesystem;

using json = nlohmann::json;

constexpr std::size_t kMaxTableBytes = std::size_t{256} * 1024;

json read_object(const std::string& path, std::string& error) {
    bool truncated = false;
    const std::string text = read_capped_file(path, kMaxTableBytes, truncated);
    if (text.empty()) {
        error = path + ": empty or unreadable";
        return {};
    }
    if (truncated) {
        error = path + ": larger than the table cap";
        return {};
    }
    try {
        return json::parse(text);
    } catch (const std::exception& e) {
        error = path + ": " + e.what();
        return {};
    }
}

std::vector<std::string> string_list(const json& value, const char* field) {
    std::vector<std::string> out;
    if (!value.contains(field) || !value[field].is_array())
        return out;
    for (const auto& item : value[field]) {
        if (item.is_string() && !item.get<std::string>().empty())
            out.push_back(item.get<std::string>());
    }
    return out;
}

// One signal row, or nothing when it cannot be used: an id, a label, a detector
// and at least one query are all required.
std::optional<ProjectSignal> parse_signal(const json& entry) {
    if (!entry.is_object())
        return std::nullopt;
    ProjectSignal signal;
    signal.id = entry.value("id", std::string());
    signal.label = entry.value("label", std::string());
    signal.file = entry.value("file", std::string());
    signal.directory = entry.value("directory", std::string());
    signal.contains = string_list(entry, "contains");
    signal.queries = string_list(entry, "queries");
    if (signal.id.empty() || signal.label.empty() || signal.queries.empty())
        return std::nullopt;
    if (signal.file.empty() && signal.directory.empty())
        return std::nullopt;
    return signal;
}

std::optional<SkillCollection> parse_collection(const json& entry) {
    if (!entry.is_object())
        return std::nullopt;
    SkillCollection collection;
    collection.name = entry.value("name", std::string());
    collection.description = entry.value("description", std::string());
    collection.queries = string_list(entry, "queries");
    collection.ids = string_list(entry, "ids");
    if (collection.name.empty() || (collection.queries.empty() && collection.ids.empty()))
        return std::nullopt;
    return collection;
}

template <typename T, typename Parser>
std::vector<T> parse_table(const std::string& path, const char* field, Parser parse,
                           std::string& error) {
    std::vector<T> table;
    const json root = read_object(path, error);
    if (!root.is_object() || !root.contains(field) || !root[field].is_array())
        return table;
    for (const auto& entry : root[field]) {
        if (auto parsed = parse(entry))
            table.push_back(std::move(*parsed));
    }
    return table;
}

// True when `needle` occurs in the file at `path`. A missing file is false.
bool file_contains(const fs::path& path, const std::string& needle) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec))
        return false;
    bool truncated = false;
    const std::string body = read_capped_file(path.string(), kMaxTableBytes, truncated);
    return body.find(needle) != std::string::npos;
}

void push_unique(std::vector<std::string>& into, std::set<std::string>& seen,
                 const std::string& value) {
    if (value.empty() || !seen.insert(value).second)
        return;
    into.push_back(value);
}

} // namespace

std::vector<ProjectSignal> load_project_signals(const std::string& path, std::string& error) {
    return parse_table<ProjectSignal>(path, "signals", parse_signal, error);
}

std::vector<SkillCollection> load_collections(const std::string& path, std::string& error) {
    return parse_table<SkillCollection>(path, "collections", parse_collection, error);
}

ProjectFacts detect_project_signals(const std::vector<ProjectSignal>& table,
                                    const std::string& project_root) {
    ProjectFacts facts;
    std::set<std::string> seen_labels;
    std::set<std::string> seen_queries;
    for (const auto& signal : table) {
        bool matched = false;
        if (!signal.file.empty()) {
            const fs::path file = fs::path(project_root) / signal.file;
            std::error_code ec;
            matched = signal.contains.empty()
                          ? fs::is_regular_file(file, ec)
                          : std::any_of(signal.contains.begin(), signal.contains.end(),
                                        [&file](const std::string& needle) {
                                            return file_contains(file, needle);
                                        });
        }
        if (!matched && !signal.directory.empty()) {
            std::error_code ec;
            matched = fs::is_directory(fs::path(project_root) / signal.directory, ec);
        }
        if (!matched)
            continue;
        push_unique(facts.labels, seen_labels, signal.label);
        for (const auto& query : signal.queries)
            push_unique(facts.queries, seen_queries, query);
    }
    return facts;
}

} // namespace agent
