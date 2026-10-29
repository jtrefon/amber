// The bundled shelves: repository facts that justify a search, and the
// editorial collections. Both are data, both are tolerant of a bad file.

#include "agent/skill_shelves.h"

#include "test_util.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace agent;

namespace {

namespace fs = std::filesystem;

struct Tree {
    explicit Tree(const std::string& name) : root("/tmp/amber_shelves_" + name) {
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root, ec);
    }
    ~Tree() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    Tree(const Tree&) = delete;
    Tree& operator=(const Tree&) = delete;
    std::string root;
};

void write(const std::string& path, const std::string& body) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream f(path, std::ios::binary);
    f << body;
}

ProjectSignal signal(const std::string& id, const std::string& label, const std::string& file,
                     std::vector<std::string> contains, std::vector<std::string> queries) {
    ProjectSignal s;
    s.id = id;
    s.label = label;
    s.file = file;
    s.contains = std::move(contains);
    s.queries = std::move(queries);
    return s;
}

} // namespace

TEST(project_signals_match_files_and_directories) {
    Tree tree("detect");
    write(tree.root + "/package.json", R"({"dependencies":{"react":"18","typescript":"5"}})");
    write(tree.root + "/tests/a.cpp", "");
    write(tree.root + "/README.md", "hello");

    std::vector<ProjectSignal> table;
    table.push_back(
        signal("typescript", "TypeScript", "package.json", {"\"typescript\""}, {"typescript"}));
    table.push_back(signal("react", "React", "package.json", {"\"react\""}, {"react", "ui"}));
    table.push_back(signal("go", "Go", "go.mod", {}, {"go"}));
    ProjectSignal dirs;
    dirs.id = "tests";
    dirs.label = "tests";
    dirs.directory = "tests";
    dirs.queries = {"testing"};
    table.push_back(dirs);

    const ProjectFacts facts = detect_project_signals(table, tree.root);
    ASSERT_EQ(facts.labels.size(), static_cast<size_t>(3));
    ASSERT_EQ(facts.labels[0], std::string("TypeScript"));
    ASSERT_EQ(facts.labels[1], std::string("React"));
    ASSERT_EQ(facts.labels[2], std::string("tests"));
    ASSERT_EQ(facts.queries.size(), static_cast<size_t>(4));
    ASSERT_EQ(facts.queries[0], std::string("typescript"));
    ASSERT_EQ(facts.queries[1], std::string("react"));
    ASSERT_EQ(facts.queries[2], std::string("ui"));
    ASSERT_EQ(facts.queries[3], std::string("testing"));
}

TEST(project_signals_with_no_facts_are_empty_not_invented) {
    Tree tree("empty");
    write(tree.root + "/README.md", "no package.json here");
    std::vector<ProjectSignal> table;
    table.push_back(signal("react", "React", "package.json", {"\"react\""}, {"react"}));
    const ProjectFacts facts = detect_project_signals(table, tree.root);
    ASSERT_TRUE(facts.labels.empty());
    ASSERT_TRUE(facts.queries.empty());
}

TEST(project_signals_loader_reads_the_shipped_table) {
    std::string error;
    const auto table = load_project_signals("data/project-signals.json", error);
    ASSERT_TRUE(error.empty());
    ASSERT(table.size() >= 8);
    bool saw_react = false;
    for (const auto& signal : table) {
        if (signal.id == "react") {
            saw_react = true;
            ASSERT_FALSE(signal.queries.empty());
        }
        ASSERT_TRUE(!signal.file.empty() || !signal.directory.empty());
    }
    ASSERT_TRUE(saw_react);
}

TEST(project_signals_loader_is_tolerant) {
    Tree tree("bad");
    std::string error;
    ASSERT_TRUE(load_project_signals(tree.root + "/missing.json", error).empty());
    ASSERT_FALSE(error.empty());

    error.clear();
    write(tree.root + "/bad.json", "{not json");
    ASSERT_TRUE(load_project_signals(tree.root + "/bad.json", error).empty());
    ASSERT_FALSE(error.empty());

    error.clear();
    write(tree.root + "/partial.json",
          R"({"signals":[{"id":"ok","label":"Ok","file":"x","queries":["x"]},{"label":"no id"}]})");
    const auto table = load_project_signals(tree.root + "/partial.json", error);
    ASSERT_TRUE(error.empty());
    ASSERT_EQ(table.size(), static_cast<size_t>(1)); // the incomplete row is dropped
}

TEST(skill_collections_load_and_validate) {
    std::string error;
    const auto shipped = load_collections("data/skill-collections.json", error);
    ASSERT_TRUE(error.empty());
    ASSERT(shipped.size() >= 4);
    ASSERT_FALSE(shipped[0].name.empty());
    ASSERT_FALSE(shipped[0].queries.empty());

    Tree tree("collections");
    write(tree.root + "/c.json",
          R"({"collections":[{"name":"Mine","description":"d","ids":["acme/skills/tdd"]},)"
          R"({"name":"Empty"}]})");
    const auto mine = load_collections(tree.root + "/c.json", error);
    ASSERT_EQ(mine.size(), static_cast<size_t>(1));
    ASSERT_EQ(mine[0].ids[0], std::string("acme/skills/tdd"));

    write(tree.root + "/broken.json", "[]");
    error.clear();
    ASSERT_TRUE(load_collections(tree.root + "/broken.json", error).empty());
    ASSERT_TRUE(error.empty()); // wrong shape is empty, not an error string
}
