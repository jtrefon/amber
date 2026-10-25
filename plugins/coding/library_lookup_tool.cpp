#include "library_lookup_tool.h"

#include "coding_library.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace agent::plugins {

namespace {

namespace fs = std::filesystem;

constexpr std::size_t kMaxMatches = 5;

std::string string_arg(const json& args, const char* key) {
    if (!args.is_object() || !args.contains(key))
        return {};
    const json& value = args[key];
    return value.is_string() ? value.get<std::string>() : std::string();
}

bool bool_arg(const json& args, const char* key) {
    return args.is_object() && args.contains(key) && args[key].is_boolean() &&
           args[key].get<bool>();
}

std::string lowered(std::string text) {
    for (char& c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// Every entry in the library, topics expanded: a name or a query has to reach a
// child as easily as a top-level entry, or the topic's "(3 entries)" is a
// promise the tool keeps.
std::vector<LibraryEntry> all_entries() {
    std::vector<LibraryEntry> entries;
    for (const auto& entry : library_entries()) {
        const bool topic = entry.topic;
        const std::string dir = fs::path(entry.path).parent_path().string();
        entries.push_back(entry);
        if (topic)
            for (auto& child : topic_entries(dir))
                entries.push_back(std::move(child));
    }
    return entries;
}

std::string usage_text() {
    return "library_lookup needs a `name` (an entry or topic from the working index), a "
           "`query` (words like OOP, SOLID, patterns, architecture, sanitizer), or `list`.\n";
}

std::string vocabulary_hint() {
    std::string out = "Try a word the library knows:";
    for (const auto& [tag, synonyms] : tag_aliases()) {
        out += " ";
        out += tag;
    }
    out += ". Or ask for `list` to see every entry.\n";
    return out;
}

std::string topic_text(const LibraryEntry& topic) {
    std::string out = read_library_text(topic.path);
    if (!out.empty() && out.back() != '\n')
        out += '\n';
    out += "\n## Entries in this topic\n\n";
    for (const auto& child : topic_entries(fs::path(topic.path).parent_path().string())) {
        out += "- ";
        out += child.name;
        out += ": ";
        out += child.trigger;
        out += '\n';
    }
    return out;
}

std::string list_text() {
    return render_library_list();
}

std::string query_text(const std::string& query) {
    const auto aliases = tag_aliases();
    const auto entries = all_entries();
    std::vector<std::pair<int, const LibraryEntry*>> scored;
    for (const auto& entry : entries) {
        const int score = entry_query_score(entry, query, aliases);
        if (score > 0)
            scored.emplace_back(score, &entry);
    }
    if (scored.empty())
        return "nothing in the library matches that. " + vocabulary_hint();
    std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first)
            return a.first > b.first;
        return a.second->name < b.second->name;
    });
    std::string out;
    for (std::size_t i = 0; i < scored.size() && i < kMaxMatches; ++i) {
        out += "- ";
        out += scored[i].second->name;
        out += ": ";
        out += scored[i].second->trigger;
        out += '\n';
    }
    out += "\nAsk for one by name to read it.\n";
    return out;
}

class LibraryLookupTool : public Tool {
public:
    std::string name() const noexcept override { return "library_lookup"; }
    std::string description() const noexcept override {
        return "Read amber's coding library: design and SOLID, patterns, architecture, C++/git/"
               "docker depth, testing and debugging. Ask by `name` (an entry the working index "
               "names), by `query` (the words people use, e.g. OOP or SOLID), or `list` for the "
               "vocabulary and every entry. The library is amber's own reference material, not "
               "the project's.";
    }
    json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties",
             {{"name",
               {{"type", "string"}, {"description", "Entry or topic name from the working index"}}},
              {"query",
               {{"type", "string"},
                {"description",
                 "Words to match against entries, tags, aliases and bodies (OOP, SOLID, patterns, "
                 "architecture, sanitizer, bisect)"}}},
              {"list",
               {{"type", "boolean"},
                {"description", "List the vocabulary and every entry name"}}}}}};
    }
    bool is_read_only() const noexcept override { return true; }
    ToolResult execute(const json& args) const override;
};

ToolResult LibraryLookupTool::execute(const json& args) const {
    if (bool_arg(args, "list"))
        return ToolResult{true, list_text(), {}, nullptr};

    const std::string name = string_arg(args, "name");
    const std::string query = string_arg(args, "query");
    if (name.empty() && query.empty())
        return ToolResult{true, usage_text(), {}, nullptr};

    const std::vector<LibraryEntry> entries = all_entries();
    if (entries.empty())
        return ToolResult{true, "the library is not available in this install\n", {}, nullptr};

    if (!name.empty()) {
        for (const auto& entry : entries) {
            if (lowered(entry.name) != lowered(name))
                continue;
            return ToolResult{
                true, entry.topic ? topic_text(entry) : read_library_text(entry.path), {}, nullptr};
        }
        return ToolResult{false, {}, "no library entry named '" + name + "'", nullptr};
    }

    return ToolResult{true, query_text(query), {}, nullptr};
}

} // namespace

std::vector<std::unique_ptr<Tool>> wrap_library_lookup() {
    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<LibraryLookupTool>());
    return tools;
}

} // namespace agent::plugins
