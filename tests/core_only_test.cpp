// The plugin-free core. This binary links only libagent_core.a and
// libagent_tools.a: if the core archive still pulled in a bundled plugin, the
// link or the archive check in `make core-only` would fail before this ran.
//
// The run proves the deployment claim: a host with no plugin set can start the
// runtime and complete an agent turn. The tools are the host's choice, so an
// empty registry is a valid configuration and the agent still answers.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "agent.h"
#include "agent/plugin_runtime.h"
#include "fake_llm.h"

namespace {

namespace fs = std::filesystem;

// Redirect every path amber keeps outside the workspace into one scratch tree.
// The test must never read the developer's real config or plugin state.
class ScratchRoots {
public:
    ScratchRoots() : dir_(fs::temp_directory_path() / "amber_core_only") {
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        override("XDG_CONFIG_HOME", dir_ / "config");
        override("XDG_STATE_HOME", dir_ / "state");
        override("XDG_CACHE_HOME", dir_ / "cache");
        override("HOME", dir_ / "home");
        save("AMBER_WORKSPACE");
        setenv("AMBER_WORKSPACE", dir_.c_str(), 1);
    }

    ~ScratchRoots() {
        restore("XDG_CONFIG_HOME");
        restore("XDG_STATE_HOME");
        restore("XDG_CACHE_HOME");
        restore("HOME");
        restore("AMBER_WORKSPACE");
        fs::remove_all(dir_);
    }

    ScratchRoots(const ScratchRoots&) = delete;
    ScratchRoots& operator=(const ScratchRoots&) = delete;

    const fs::path& path() const { return dir_; }

private:
    struct Saved {
        std::string name;
        std::optional<std::string> value;
    };

    void save(const std::string& name) {
        const char* value = std::getenv(name.c_str());
        saved_.push_back({name, value ? std::optional<std::string>(value) : std::nullopt});
    }
    void override(const std::string& name, const fs::path& value) {
        save(name);
        setenv(name.c_str(), value.c_str(), 1);
    }
    void restore(const std::string& name) {
        for (const auto& entry : saved_) {
            if (entry.name != name)
                continue;
            if (entry.value)
                setenv(name.c_str(), entry.value->c_str(), 1);
            else
                unsetenv(name.c_str());
        }
    }

    fs::path dir_;
    std::vector<Saved> saved_;
};

int failures = 0;

void check(bool condition, const char* what) {
    if (condition)
        return;
    std::fprintf(stderr, "core-only: %s\n", what);
    ++failures;
}

} // namespace

int main() {
    ScratchRoots roots;
    agent::Workspace::set_root(roots.path().string());

    // A system prompt in the scratch tree so the run does not depend on the
    // working directory or on the shipped prompt files.
    const fs::path prompt = roots.path() / "system.md";
    {
        std::ofstream out(prompt);
        out << "You are a minimal test agent.\n";
    }

    agent::Config cfg;
    cfg.model = "fake";
    cfg.stream = false;
    cfg.mode = agent::AgentMode::Write;
    cfg.max_tool_iterations = 3;
    cfg.system_prompt_path = prompt.string();

    agent::ToolRegistry registry;
    agent::Workspace workspace;
    agent::PluginRuntime runtime(registry, cfg, workspace);
    runtime.start();
    check(runtime.list().empty(), "a plugin-free runtime starts with no plugins");
    check(!runtime.has("tool_read"), "the bundled tool plugins are not registered");
    check(!runtime.has("coding"), "the bundled domain plugins are not registered");

    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeReply scripted;
    scripted.content = "core is alive";
    fake->script.push_back(scripted);

    agent::Agent agent(cfg, registry, {}, agent::AgentDeps{{}, {}, {}, {}, std::move(fake), {}, {}});
    const std::string answer = agent.run("hello");
    check(answer == "core is alive", "a plugin-free agent completes a turn");
    check(registry.empty(), "the core registered no tools on its own");

    if (failures == 0)
        std::printf("core-only: a plugin-free core boots and runs an agent\n");
    return failures == 0 ? 0 : 1;
}
