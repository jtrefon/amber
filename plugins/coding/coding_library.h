#ifndef AGENT_PLUGINS_CODING_LIBRARY_H
#define AGENT_PLUGINS_CODING_LIBRARY_H

#include <map>
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
    // The names people use for this entry that are not its tags: "OOP", "SOLID",
    // "event sourcing". Retrieval matches these, so a question phrased in the
    // reader's vocabulary reaches the entry.
    std::vector<std::string> aliases;
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

// Synonyms the vocabulary declares for a tag (`also: OOP, SOLID` on the tag's
// line), keyed by the bare tag. One place to teach the library the words a
// reader uses; every entry carrying the tag inherits them.
std::map<std::string, std::vector<std::string>> tag_aliases();

// The tags whose entries are part of the craft layer: design, patterns and
// architecture. They are indexed for every project, because they answer what the
// *task* needs rather than what the project is -- a project tag cannot select
// them, and a fresh project has no tags at all.
const std::vector<std::string>& craft_tags();

// How well `entry` answers `query`, 0 when it does not. Whole words and
// (for words of four characters or more) prefixes match; a tag's declared
// synonyms match every entry carrying that tag. Name and trigger hits outrank
// body hits, so the entry *about* the query comes before one that mentions it.
int entry_query_score(const LibraryEntry& entry, const std::string& query,
                      const std::map<std::string, std::vector<std::string>>& aliases);

// True when the entry answers the query (a score above zero).
bool entry_matches_query(const LibraryEntry& entry, const std::string& query,
                         const std::map<std::string, std::vector<std::string>>& aliases);

// The library's own directory: resolved next to the binary first, then relative
// to the working directory, like every other prompt file.
std::string library_dir();

// The working index: one trigger line per selected entry, heaviest first, as a
// per-turn block. The craft layer is always present; a project's tags add the
// domain entries. Empty only when the library itself is missing.
std::string render_working_index();

// The skill body: the vocabulary and how to reach an entry. Small and stable --
// the entries themselves load on demand.
std::string render_library_skill_body();

// The whole library as text: the vocabulary, then one line per entry (topics
// expanded). The `/library list` command and the tool's `list` mode share it.
std::string render_library_list();

} // namespace agent::plugins

#endif // AGENT_PLUGINS_CODING_LIBRARY_H
