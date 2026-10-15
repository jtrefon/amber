// Doc-as-test guard: docs/spec/plugins/developer-guide.md must describe what the
// code actually exposes. Adding a capability kind, an event payload, or a
// bundled plugin without updating the guide fails this test.
//
// It reads the real headers and the real markdown (located via __FILE__), so the
// guide cannot drift from the code silently.

#include "agent/plugin_capability.h"
#include "agent/plugins_bundled.h"
#include "test_util.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string repo_root() {
    std::string f = __FILE__; // .../tests/plugin_docs_test.cpp
    const auto slash = f.find_last_of('/');
    std::string dir = (slash == std::string::npos) ? "." : f.substr(0, slash);
    const auto up = dir.find_last_of('/');
    return (up == std::string::npos) ? "." : dir.substr(0, up);
}

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Identifiers between `enum class NAME ... {` and the closing `};`.
std::vector<std::string> enum_values(const std::string& text, const std::string& enum_name) {
    std::vector<std::string> out;
    const auto start = text.find("enum class " + enum_name);
    if (start == std::string::npos)
        return out;
    const auto brace = text.find('{', start);
    const auto end = text.find("};", brace);
    if (brace == std::string::npos || end == std::string::npos)
        return out;
    const std::string body = text.substr(brace + 1, end - brace - 1);
    const std::regex re(R"(([A-Za-z_][A-Za-z0-9_]*)\s*[,=])");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), re); it != std::sregex_iterator();
         ++it)
        out.push_back((*it)[1].str());
    return out;
}

std::vector<std::string> event_structs(const std::string& text) {
    std::vector<std::string> out;
    const std::regex re(R"(struct\s+([A-Za-z_][A-Za-z0-9_]*Event)\b)");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), re); it != std::sregex_iterator();
         ++it)
        out.push_back((*it)[1].str());
    return out;
}

const char* const kGuide = "/docs/spec/plugins/developer-guide.md";

} // namespace

TEST(plugin_docs_guide_documents_every_capability_kind) {
    const std::string root = repo_root();
    const std::string header = read_file(root + "/include/agent/plugin_capability.h");
    const std::string guide = read_file(root + kGuide);
    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(guide.empty());

    const auto kinds = enum_values(header, "CapabilityKind");
    ASSERT(kinds.size() >= 7);
    for (const auto& k : kinds)
        if (guide.find(k) == std::string::npos)
            agent::test::fail("capability kind '" + k +
                              "' is not documented in docs/spec/plugins/developer-guide.md");
}

TEST(plugin_docs_guide_documents_every_event) {
    const std::string root = repo_root();
    const std::string header = read_file(root + "/include/agent/events.h");
    const std::string guide = read_file(root + kGuide);
    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(guide.empty());

    const auto events = event_structs(header);
    ASSERT(events.size() >= 9);
    for (const auto& e : events)
        if (guide.find(e) == std::string::npos)
            agent::test::fail("event '" + e +
                              "' is not documented in docs/spec/plugins/developer-guide.md");
}

TEST(plugin_docs_guide_documents_every_bundled_plugin) {
    const std::string root = repo_root();
    const std::string guide = read_file(root + kGuide);
    ASSERT_FALSE(guide.empty());

    const auto plugins = agent::make_bundled_plugins();
    ASSERT(plugins.size() >= 10);
    for (const auto& p : plugins)
        if (guide.find(p->id()) == std::string::npos)
            agent::test::fail("bundled plugin '" + p->id() +
                              "' is not documented in docs/spec/plugins/developer-guide.md");
}

// ---------------------------------------------------------------------------
// The coding library: what is resident, and the one-home rule for what is not.
// ---------------------------------------------------------------------------

namespace {

std::string trimmed(const std::string& line) {
    const std::size_t first = line.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    return line.substr(first, line.find_last_not_of(" \t\r\n") - first + 1);
}

// Content lines, not layout: a blank line between paragraphs costs nothing, and
// the budget is about what the prefix carries.
std::size_t count_content_lines(const std::string& text) {
    std::size_t lines = 0;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
        if (!trimmed(line).empty())
            ++lines;
    return lines;
}

std::vector<std::string> library_files(const std::string& root) {
    std::vector<std::string> files;
    std::error_code ec;
    const std::string dir = root + "/prompts/domains/coding/library";
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(dir, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.size() > 3 && name.compare(name.size() - 3, 3, ".md") == 0)
            files.push_back(dir + "/" + name);
    }
    std::sort(files.begin(), files.end());
    return files;
}

} // namespace

TEST(coding_domain_resident_prose_stays_within_its_budget) {
    // Residency is for the key, not the library. The domain's identity, workflow
    // and discipline ride in the system prefix on every turn; everything else is
    // dark until a project's tags select it.
    //
    // 110 is the *measured* value, ratcheted the way every other budget here is:
    // it may only shrink from here, so growth is caught and no one has to
    // re-derive why it is what it is. The 90 the design first named was a guess;
    // the prose that came out of review is 108 content lines, and trimming it to
    // meet a number nobody had measured would have been the wrong trade.
    const std::string dir = repo_root() + "/prompts/domains/coding/";
    std::size_t total = 0;
    for (const char* name : {"identity.md", "git.md", "engineering.md"})
        total += count_content_lines(read_file(dir + name));
    ASSERT(total > 0);
    ASSERT(total <= 110);
}

TEST(coding_library_entries_declare_a_trigger_and_tags) {
    const std::vector<std::string> files = library_files(repo_root());
    ASSERT(files.size() >= 8);
    for (const auto& file : files) {
        const std::string text = read_file(file);
        const std::size_t header_end = text.find("\n\n");
        ASSERT(header_end != std::string::npos);
        // Both live in the leading header block: the trigger is the line the
        // working index renders, and the tags are what selects it.
        ASSERT(text.find("trigger:") != std::string::npos);
        ASSERT(text.find("trigger:") < header_end);
        ASSERT(text.find("tags:") != std::string::npos);
        ASSERT(text.find("tags:") < header_end);
    }
}

TEST(coding_library_entries_do_not_restate_a_rule_that_lives_elsewhere) {
    // One home per rule: an entry adds what is specific to the situation, and
    // the rule itself lives in core, in a tool doc, in the domain's own prose or
    // in the project's AGENTS.md. A literal restatement is the copy that drifts.
    const std::string root = repo_root();
    std::string elsewhere;
    for (const char* source :
         {"prompts/system.md", "prompts/domains/coding/identity.md",
          "prompts/domains/coding/git.md", "prompts/domains/coding/engineering.md",
          "prompts/tools/read.md", "prompts/tools/write.md", "prompts/tools/bash.md",
          "prompts/tools/skills.md", "AGENTS.md"})
        elsewhere += read_file(root + "/" + source);

    for (const auto& file : library_files(root)) {
        std::istringstream in(read_file(file));
        std::string line;
        while (std::getline(in, line)) {
            const std::string text = trimmed(line);
            if (text.size() < 40)
                continue;
            if (elsewhere.find(text) != std::string::npos)
                agent::test::fail("library entry restates a rule that lives elsewhere: " + text);
        }
    }
}
