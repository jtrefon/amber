// Doc-as-test guard: docs/spec/plugins/developer-guide.md must describe what the
// code actually exposes. Adding a capability kind, an event payload, or a
// bundled plugin without updating the guide fails this test.
//
// It reads the real headers and the real markdown (located via __FILE__), so the
// guide cannot drift from the code silently.

#include "agent/plugin_capability.h"
#include "agent/plugins_bundled.h"
#include "test_util.h"

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
    for (auto it = std::sregex_iterator(body.begin(), body.end(), re);
         it != std::sregex_iterator(); ++it)
        out.push_back((*it)[1].str());
    return out;
}

std::vector<std::string> event_structs(const std::string& text) {
    std::vector<std::string> out;
    const std::regex re(R"(struct\s+([A-Za-z_][A-Za-z0-9_]*Event)\b)");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), re);
         it != std::sregex_iterator(); ++it)
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
