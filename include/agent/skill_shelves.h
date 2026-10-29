#ifndef AMBER_AGENT_SKILL_SHELVES_H
#define AMBER_AGENT_SKILL_SHELVES_H

#include <string>
#include <vector>

namespace agent {

// One row of `data/project-signals.json`: a fact about the repository and the
// queries it justifies. Detection is a substring test, never a model call.
struct ProjectSignal {
    std::string id;
    std::string label;                 // shown as a reason: "React"
    std::string file;                  // root file to search, e.g. "package.json"
    std::string directory;             // or a root directory, e.g. "tests"
    std::vector<std::string> contains; // any of these substrings in `file`
    std::vector<std::string> queries;
};

// What the repository justifies: the reasons to show and the queries to run,
// both in table order and deduplicated.
struct ProjectFacts {
    std::vector<std::string> labels;
    std::vector<std::string> queries;
};

// A bundled, editable shelf: a name, a reason and the queries or ids it holds.
struct SkillCollection {
    std::string name;
    std::string description;
    std::vector<std::string> queries;
    std::vector<std::string> ids;
};

// Both loaders are tolerant by design: a missing file yields an empty table, a
// malformed one yields an empty table plus `error`, and neither throws. The
// shelf simply has nothing to show, which the UI says.
struct CuratedOwner {
    std::string name;  // the registry source owner
    std::string label; // what the shelf shows
};

std::vector<ProjectSignal> load_project_signals(const std::string& path, std::string& error);
std::vector<CuratedOwner> load_curated_owners(const std::string& path, std::string& error);
std::vector<SkillCollection> load_collections(const std::string& path, std::string& error);

// Reads the repository at `project_root` with `table` and reports what it found.
ProjectFacts detect_project_signals(const std::vector<ProjectSignal>& table,
                                    const std::string& project_root);

} // namespace agent

#endif // AMBER_AGENT_SKILL_SHELVES_H
