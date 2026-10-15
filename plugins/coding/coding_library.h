#ifndef AGENT_PLUGINS_CODING_LIBRARY_H
#define AGENT_PLUGINS_CODING_LIBRARY_H

#include <string>
#include <vector>

namespace agent::plugins {

// One library entry, parsed from its file. `trigger` is the line the working
// index renders; an entry with no trigger has nothing to render and is dropped
// by the caller.
struct LibraryEntry {
    std::string trigger;
    std::vector<std::string> tags;
    std::string path; // where the body is, so the agent can read it on demand
};

// Parse one entry's header. Pure -- no I/O -- so the format is testable on its
// own, which is the point: the index renders exactly what the header declares.
LibraryEntry parse_library_entry(const std::string& path, const std::string& text);

// True when the entry carries one of `wanted`. Tags compare without their '#',
// because the index writes them with it and an entry may write them either way.
bool entry_matches(const LibraryEntry& entry, const std::vector<std::string>& wanted);

// The tags a project index selects, read from its "## Library selection"
// section. Pure for the same reason.
std::vector<std::string> selected_tags_from(const std::string& index_body);

// The library's own directory: resolved next to the binary first, then relative
// to the working directory, like every other prompt file.
std::string library_dir();

// The working index: one trigger line per selected entry, as a per-turn block.
// Empty when nothing is selected -- the library is dark until a project asks
// for a part of it.
std::string render_working_index();

} // namespace agent::plugins

#endif // AGENT_PLUGINS_CODING_LIBRARY_H
