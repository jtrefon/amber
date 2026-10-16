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
// per selected entry, and no more than this many of them. Ranking is what makes
// the cap tolerable -- the heaviest entries are the ones that survive it.
constexpr std::size_t kMaxEntries = 25;

// The default weight, and the ceiling a header may claim. A topic outranks an
// entry by default because pulling it reaches many entries at once.
constexpr int kDefaultWeight = 50;
constexpr int kTopicWeight = 60;

constexpr const char* kLibraryRel = "prompts/domains/coding/library";
constexpr const char* kTopicManifest = "topic.md";
constexpr const char* kVocabulary = "tags.md";

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

bool is_markdown(const std::string& name) {
    return name.size() > 3 && name.compare(name.size() - 3, 3, ".md") == 0 && name[0] != '.';
}

std::string stem_of(const std::string& name) {
    return is_markdown(name) ? name.substr(0, name.size() - 3) : name;
}

std::string to_lower(std::string text) {
    for (char& c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// A weight a header may carry: plain digits, clamped. Anything else is the
// default rather than an error -- a malformed weight should not hide knowledge.
int parse_weight(const std::string& value) {
    if (value.empty() || value.size() > 3)
        return kDefaultWeight;
    int weight = 0;
    for (char c : value) {
        if (c < '0' || c > '9')
            return kDefaultWeight;
        weight = weight * 10 + (c - '0');
    }
    return std::min(weight, 100);
}

// How many entries a topic holds: its own files, minus the manifest and the
// vocabulary, if they live inside it.
std::size_t count_children(const fs::path& dir) {
    std::size_t count = 0;
    std::error_code ec;
    for (const auto& item : fs::directory_iterator(dir, ec)) {
        if (!item.is_regular_file(ec))
            continue;
        const std::string name = item.path().filename().string();
        if (is_markdown(name) && name != kTopicManifest && name != kVocabulary)
            ++count;
    }
    return count;
}

LibraryEntry read_entry(const std::string& path, const std::string& fallback_name) {
    LibraryEntry entry = parse_library_entry(path, read_text(path));
    if (entry.name.empty())
        entry.name = fallback_name;
    return entry;
}

void collect_entries(const std::string& dir, std::vector<LibraryEntry>& out) {
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec)
        return;
    for (const auto& item : it) {
        const std::string name = item.path().filename().string();
        if (name.empty() || name[0] == '.' || name == kVocabulary)
            continue; // the vocabulary is a document, not an entry
        if (item.is_regular_file(ec) && is_markdown(name)) {
            out.push_back(read_entry(item.path().string(), stem_of(name)));
            continue;
        }
        if (!item.is_directory(ec))
            continue;
        // A topic is a directory whose manifest declares its line. Without one
        // it is not selectable: a directory nobody wrote a trigger for would
        // otherwise appear as a blank line.
        const fs::path manifest = item.path() / kTopicManifest;
        if (!fs::is_regular_file(manifest, ec))
            continue;
        LibraryEntry topic = read_entry(manifest.string(), name);
        topic.name = name;
        topic.topic = true;
        topic.children = count_children(item.path());
        if (topic.weight == kDefaultWeight)
            topic.weight = kTopicWeight;
        out.push_back(std::move(topic));
    }
}

bool heavier_first(const LibraryEntry& a, const LibraryEntry& b) {
    if (a.weight != b.weight)
        return a.weight > b.weight;
    return a.name < b.name;
}

void append_line(std::string& out, const LibraryEntry& entry) {
    // Piecewise: a concatenation that builds a temporary per entry is the shape
    // the linter is right about, and this line is a per-turn cost.
    out += "- ";
    out += entry.trigger;
    if (entry.topic && entry.children > 0) {
        out += " (";
        out += std::to_string(entry.children);
        out += " entries)";
    }
    out += " — `";
    out += entry.path;
    out += "`\n";
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
        if (key == "name")
            entry.name = value;
        else if (key == "trigger")
            entry.trigger = value;
        else if (key == "tags")
            entry.tags = split_tags(value);
        else if (key == "weight")
            entry.weight = parse_weight(value);
    }
    return entry;
}

std::vector<LibraryEntry> library_entries() {
    std::vector<LibraryEntry> entries;
    const std::string dir = library_dir();
    if (!dir.empty())
        collect_entries(dir, entries);
    std::sort(entries.begin(), entries.end(), heavier_first);
    return entries;
}

bool entry_matches(const LibraryEntry& entry, const std::vector<std::string>& wanted) {
    return std::any_of(entry.tags.begin(), entry.tags.end(), [&wanted](const std::string& tag) {
        const std::string bare = tag[0] == '#' ? tag.substr(1) : tag;
        return std::find(wanted.begin(), wanted.end(), bare) != wanted.end();
    });
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
    std::vector<LibraryEntry> selected;
    for (auto& entry : library_entries())
        if (!entry.trigger.empty() && entry_matches(entry, wanted))
            selected.push_back(std::move(entry));
    if (selected.empty())
        return {};

    std::string out = "## Library entries this project selects\n\n"
                      "Trigger lines only: read the file when the situation matches.\n\n";
    const std::size_t shown = std::min(selected.size(), kMaxEntries);
    for (std::size_t i = 0; i < shown; ++i)
        append_line(out, selected[i]);
    return out;
}

} // namespace agent::plugins
