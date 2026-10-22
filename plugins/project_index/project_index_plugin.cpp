#include "project_index_plugin.h"

#include "agent/extensions.h"
#include "agent/prompt.h"
#include "agent/workspace.h"
#include "index_store.h"
#include "index_tools.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <vector>

namespace agent::plugins {

namespace {

namespace fs = std::filesystem;

// The block rides in the system prompt on every turn, so its size is a real
// cost. The index is meant to be a handful of sections; 4 KB is generous, and
// past that the tail is elided with a marker that says where the rest lives.
constexpr std::size_t kMaxBytes = std::size_t{4} * 1024;

// A file the index was derived from, with the hash recorded when it was
// written. A later read compares the two and reports the difference rather than
// presenting facts that may be out of date as current.
struct Source {
    std::string path;
    std::string hash;
};

std::string trim(const std::string& text) {
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

// One `path@hash` item, the shape the provenance line carries. A malformed item
// is dropped: an entry nobody can check is not a provenance claim.
void add_source(std::vector<Source>& out, const std::string& item) {
    const std::string token = trim(item);
    const std::size_t at = token.rfind('@');
    if (at == std::string::npos || at == 0 || at + 1 >= token.size())
        return;
    out.push_back(Source{token.substr(0, at), token.substr(at + 1)});
}

// The provenance line: `sources: docker-compose.yml@1f2e3d4c, api.sln@9a8b7c6d`.
// Parsed tolerantly, because the line is written by a tool and edited by hand.
std::vector<Source> parse_sources(const std::string& body) {
    std::vector<Source> sources;
    std::istringstream lines(body);
    std::string line;
    while (std::getline(lines, line)) {
        const std::size_t at = line.find("sources:");
        if (at == std::string::npos)
            continue;
        std::istringstream items(line.substr(at + std::string("sources:").size()));
        std::string item;
        while (std::getline(items, item, ','))
            add_source(sources, item);
    }
    return sources;
}

// Which recorded sources no longer match the disk. A file that cannot be read
// counts as changed: "cannot check" is not "fine".
std::vector<std::string> moved_sources(const std::vector<Source>& sources) {
    std::vector<std::string> moved;
    for (const auto& source : sources) {
        // The source list comes from a file the model writes, so it is confined
        // like any other path: a source that climbs out of the workspace cannot
        // be checked, and "cannot check" is not "fine".
        std::string resolved;
        std::string error;
        const std::string current = Workspace::confine(source.path, resolved, error)
                                        ? ProjectIndexPlugin::hash_file(resolved)
                                        : std::string();
        if (current != source.hash)
            moved.push_back(source.path + "@" + source.hash);
    }
    return moved;
}

std::string render_block(const std::string& body, bool truncated,
                         const std::vector<std::string>& moved) {
    std::string out = "# Project index\n\n";
    out += "Amber's own notes about this project, confirmed by the user: how it runs, where "
           "its parts and artefacts live, and which library entries apply. It is derived "
           "material, so the project's AGENTS.md outranks it where they disagree.\n\n";
    out += body;
    if (!out.empty() && out.back() != '\n')
        out += '\n';
    if (truncated)
        out += "\n[truncated at 4 KB — read the index file for the rest]\n";
    if (!moved.empty()) {
        out += "\nderived from ";
        for (std::size_t i = 0; i < moved.size(); ++i) {
            if (i > 0)
                out += ", ";
            out += moved[i];
        }
        out += ", changed since — the facts above may be stale.\n";
    }
    return out;
}

// 8 hex characters, folded high and low: a change detector, not a security
// primitive.
std::string hex8(std::uint32_t value) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string out(8, '0');
    for (int i = 7; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kHex[value & 0xF];
        value >>= 4;
    }
    return out;
}

} // namespace

void ProjectIndexPlugin::shutdown() {
    compression_done_.release();
    block_.clear();
    loaded_ = false;
}

bool ProjectIndexPlugin::initialize(const PluginContext& ctx) {
    // A *failed* compression leaves the context alone, so re-reading then would
    // move a prefix that did not need to move. Only a compression that happened
    // is the free moment.
    compression_done_ =
        Events(ctx.event_bus)
            .subscribe<CompressionCompletedEvent>([this](const CompressionCompletedEvent& done) {
                if (done.success)
                    refresh();
            });
    return true;
}

void ProjectIndexPlugin::refresh() {
    block_.clear();
    loaded_ = false;
}

std::string ProjectIndexPlugin::file_path() {
    const std::string dir = Workspace::state_dir();
    return dir.empty() ? std::string() : (fs::path(dir) / "project-index.md").string();
}

std::string ProjectIndexPlugin::hash_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    std::uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : ss.str()) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return hex8(static_cast<std::uint32_t>(h) ^ static_cast<std::uint32_t>(h >> 32));
}

std::string ProjectIndexPlugin::load_once() {
    if (loaded_)
        return block_;
    loaded_ = true;
    const std::string file = file_path();
    if (file.empty())
        return block_;
    bool truncated = false;
    // codeql[cpp/path-injection] lgtm[cpp/path-injection] — the index file under the project's
    // own XDG state root, the same path the writer above uses.
    const std::string body = read_capped_file(file, kMaxBytes, truncated);
    if (body.find_first_not_of(" \t\r\n") == std::string::npos)
        return block_; // an empty file says nothing, and an empty block is skipped
    block_ = render_block(body, truncated, moved_sources(parse_sources(body)));
    return block_;
}

std::vector<std::unique_ptr<Capability>> ProjectIndexPlugin::capabilities() {
    std::vector<std::unique_ptr<Capability>> caps;
    // Three contributions, one plugin: the block that carries the index, the
    // tools that write it, and the command that shows or re-reads it. They
    // arrive and leave together, which is the point -- a user who switches the
    // index off should not be left with tools that write a file nothing reads.
    caps.push_back(std::make_unique<PromptBlockCapability>(
        "project_index", prompt_priority::kProjectIndex, [this] { return load_once(); },
        PromptPlacement::System));

    caps.push_back(std::make_unique<ToolCapability>(
        "project_index_tools", [](PluginServices&) { return make_index_tools(); },
        ToolCapability::Meta{{{"index_init", {"indexing", ToolRole::Write}},
                              {"index_add", {"indexing", ToolRole::Write}},
                              {"index_update", {"indexing", ToolRole::Write}},
                              {"index_remove", {"indexing", ToolRole::Write}},
                              // Other, not Read: the audit reasons about what can
                              // bring *workspace* content back, and this reads
                              // amber's own note about the project.
                              {"index_show", {"reading", ToolRole::Other}}}}));

    CommandSpec spec;
    spec.name = "index";
    spec.help = "The project index: amber's own notes about this project";
    spec.man = "Usage: /index show — print the index file. /index refresh — re-read it now; the "
               "block in the system prompt is frozen for the session, and this is what moves it.";
    spec.subtree = json::parse(R"({
        "show": {"help": "Print the project index", "man": "Usage: /index show"},
        "refresh": {"help": "Re-read the index now", "man": "Usage: /index refresh"}
    })");
    spec.handlers["show"] = [](const std::string&) {
        const std::string body = index_store::read_all();
        return body.empty() ? std::string("no project index yet — amber can write one") : body;
    };
    spec.handlers["refresh"] = [this](const std::string&) {
        refresh();
        return load_once().empty() ? std::string("no project index yet")
                                   : std::string("index re-read: the block now reflects the file");
    };
    caps.push_back(std::make_unique<CommandCapability>(std::move(spec)));
    return caps;
}

} // namespace agent::plugins
