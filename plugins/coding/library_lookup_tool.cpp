#include "library_lookup_tool.h"

#include "coding_library.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

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
    return "library_lookup needs a `name` (an entry or topic from the working index) or a "
           "`query`.\n";
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

bool matches(const LibraryEntry& entry, const std::string& query) {
    if (lowered(entry.trigger).find(query) != std::string::npos)
        return true;
    if (lowered(entry.name).find(query) != std::string::npos)
        return true;
    if (lowered(read_library_text(entry.path)).find(query) != std::string::npos)
        return true;
    return std::any_of(entry.tags.begin(), entry.tags.end(), [&query](const std::string& tag) {
        return lowered(tag).find(query) != std::string::npos;
    });
}

class LibraryLookupTool : public Tool {
public:
    std::string name() const noexcept override { return "library_lookup"; }
    std::string description() const noexcept override {
        return "Read amber's coding library: an entry's full text by name (as the working index "
               "names it), or the entries matching a query. The library is amber's own reference "
               "material, not the project's.";
    }
    json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties",
             {{"name",
               {{"type", "string"}, {"description", "Entry or topic name from the working index"}}},
              {"query",
               {{"type", "string"},
                {"description", "Words to match against entries, tags and bodies"}}}}}};
    }
    bool is_read_only() const noexcept override { return true; }
    ToolResult execute(const json& args) const override;
};

ToolResult LibraryLookupTool::execute(const json& args) const {
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

    const std::string wanted = lowered(query);
    std::string out;
    std::size_t found = 0;
    for (const auto& entry : entries) {
        if (!matches(entry, wanted))
            continue;
        out += "- ";
        out += entry.name;
        out += ": ";
        out += entry.trigger;
        out += '\n';
        if (++found >= kMaxMatches)
            break;
    }
    if (out.empty())
        return ToolResult{true, "nothing in the library matches that\n", {}, nullptr};
    out += "\nAsk for one by name to read it.\n";
    return ToolResult{true, out, {}, nullptr};
}

} // namespace

std::vector<std::unique_ptr<Tool>> wrap_library_lookup() {
    std::vector<std::unique_ptr<Tool>> tools;
    tools.push_back(std::make_unique<LibraryLookupTool>());
    return tools;
}

} // namespace agent::plugins
