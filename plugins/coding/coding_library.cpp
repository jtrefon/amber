#include "coding_library.h"

#include "agent/data_path.h"
#include "plugins/project_index/index_store.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace agent::plugins {

namespace {

namespace fs = std::filesystem;

// The working index rides in every turn, so its size is a real cost: one line
// per selected entry, and no more than this many of them. An entry that never
// earns its line is a line that should be dark.
constexpr std::size_t kMaxEntries = 25;

constexpr const char* kLibraryRel = "prompts/domains/coding/library";

std::string trim(const std::string& text) {
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

std::vector<std::string> split_tags(const std::string& value) {
    std::vector<std::string> tags;
    std::istringstream in(value);
    std::string item;
    while (in >> item) {
        std::string tag = trim(item);
        while (!tag.empty() && (tag.back() == ',' || tag.back() == ';'))
            tag.pop_back();
        if (!tag.empty() && tag[0] == '#')
            tag.erase(0, 1);
        if (!tag.empty())
            tags.push_back(tag);
    }
    return tags;
}

std::string read_text(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool is_library_file(const fs::directory_entry& entry) {
    const std::string name = entry.path().filename().string();
    return name.size() > 3 && name.compare(name.size() - 3, 3, ".md") == 0 && name[0] != '.';
}

std::string to_lower(std::string text) {
    for (char& c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

} // namespace

LibraryEntry parse_library_entry(const std::string& path, const std::string& text) {
    LibraryEntry entry;
    entry.path = path;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const std::string trimmed = trim(line);
        if (trimmed.empty())
            break; // the body starts here; the header is the leading block
        const std::size_t colon = trimmed.find(':');
        if (colon == std::string::npos)
            break; // not a header line: the header is over
        const std::string key = to_lower(trim(trimmed.substr(0, colon)));
        const std::string value = trim(trimmed.substr(colon + 1));
        if (key == "trigger")
            entry.trigger = value;
        else if (key == "tags")
            entry.tags = split_tags(value);
    }
    return entry;
}

bool entry_matches(const LibraryEntry& entry, const std::vector<std::string>& wanted) {
    for (const auto& tag : entry.tags) {
        const std::string bare = tag[0] == '#' ? tag.substr(1) : tag;
        if (std::find(wanted.begin(), wanted.end(), bare) != wanted.end())
            return true;
    }
    return false;
}

std::vector<std::string> selected_tags_from(const std::string& index_body) {
    std::vector<std::string> tags;
    const std::size_t at = index_body.find("## Library selection");
    if (at == std::string::npos)
        return tags;
    const std::size_t end = index_body.find("\n## ", at + 1);
    std::istringstream lines(
        index_body.substr(at, end == std::string::npos ? std::string::npos : end - at));
    std::string line;
    bool first = true;
    while (std::getline(lines, line)) {
        if (first) {
            first = false; // the heading itself
            continue;
        }
        for (auto& tag : split_tags(line))
            tags.push_back(std::move(tag));
    }
    return tags;
}

std::string library_dir() {
    const std::string near_binary = exe_dir();
    if (!near_binary.empty()) {
        const std::string candidate = near_binary + "/" + kLibraryRel;
        std::error_code ec;
        if (fs::is_directory(candidate, ec))
            return candidate;
    }
    std::error_code ec;
    return fs::is_directory(kLibraryRel, ec) ? std::string(kLibraryRel) : std::string();
}

std::string render_working_index() {
    const std::vector<std::string> wanted = selected_tags_from(index_store::read_all());
    if (wanted.empty())
        return {};
    const std::string dir = library_dir();
    if (dir.empty())
        return {};
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec)
        return {};
    std::vector<std::string> names;
    for (const auto& item : it)
        if (item.is_regular_file(ec) && is_library_file(item))
            names.push_back(item.path().filename().string());
    std::sort(names.begin(), names.end());

    std::string out;
    std::size_t shown = 0;
    for (const auto& name : names) {
        const std::string path = dir + "/" + name;
        const LibraryEntry entry = parse_library_entry(path, read_text(path));
        if (entry.trigger.empty() || !entry_matches(entry, wanted))
            continue;
        if (shown == 0) {
            out = "## Library entries this project selects\n\n"
                  "Trigger lines only: read the file when the situation matches.\n\n";
        }
        out += "- " + entry.trigger + " — `" + path + "`\n";
        if (++shown >= kMaxEntries)
            break;
    }
    return out;
}

} // namespace agent::plugins
