#ifndef AGENT_PLUGINS_CODING_LIBRARY_H
#define AGENT_PLUGINS_CODING_LIBRARY_H

#include <string>
#include <vector>

namespace agent::plugins {

// One library entry, parsed from its file. `trigger` is the line the working
// index renders; an entry with no trigger has nothing to render and is dropped
// by the caller.
struct LibraryEntry {
    std::string name; // the file's stem, or a topic's directory name
    std::string trigger;
    std::vector<std::string> tags;
    std::string path;         // where the body is
    int weight = 50;          // heavier entries survive the working-index cap first
    bool topic = false;       // a directory manifest: one line for many entries
    std::size_t children = 0; // entries inside a topic, for the line's sake
};

// Parse one entry (or topic manifest) header. Pure -- no I/O -- so the format is
// testable on its own, which is the point: the index renders exactly what the
// header declares.
LibraryEntry parse_library_entry(const std::string& path, const std::string& text);

// The library's entries: one per markdown file, plus one per topic directory --
// whose `topic.md` carries its line and whose children stay dark until looked
// up. Sorted for display (weight, then name).
std::vector<LibraryEntry> library_entries();

// The entries inside one topic directory, sorted for display. Empty for a path
// that is not a directory.
std::vector<LibraryEntry> topic_entries(const std::string& dir);

// An entry's full text, header included, or empty when the file is gone.
std::string read_library_text(const std::string& path);

// True when the entry carries one of `wanted`. Tags compare without their '#',
// because the index writes them with it and an entry may write them either way.
bool entry_matches(const LibraryEntry& entry, const std::vector<std::string>& wanted);

// The tags a project index selects, read from its "## Library selection"
// section. Pure for the same reason.
std::vector<std::string> selected_tags_from(const std::string& index_body);

// The library's own directory: resolved next to the binary first, then relative
// to the working directory, like every other prompt file.
std::string library_dir();

// The working index: one trigger line per selected entry, heaviest first, as a
// per-turn block. Empty when nothing is selected -- the library is dark until a
// project asks for a part of it.
std::string render_working_index();

} // namespace agent::plugins

#endif // AGENT_PLUGINS_CODING_LIBRARY_H
