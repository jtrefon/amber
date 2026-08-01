#include "agent/plugin.h"
#include "agent/tools.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <sys/stat.h>
#include <fstream>
#include <iostream>
#include <string>

// Minimal test framework (mirrors tests/completions_test.cpp).
#define TEST(name) void name()
#define ASSERT(cond) do { \
    if (!(cond)) { std::cerr << "FAIL: " << #cond << "\n"; failed++; } \
} while(0)
#define REQUIRE(cond) do { \
    if (!(cond)) { std::cerr << "FAIL: " << #cond << "\n"; failed++; return; } \
} while(0)
#define ASSERT_EQ(a,b) do { \
    if ((a) != (b)) { std::cerr << "FAIL: " << #a << " == " << #b << "  got: " << (a) << " expected: " << (b) << "\n"; failed++; } \
} while(0)

int failed = 0;

namespace {

struct EnvGuard {
    std::string saved_xdg;
    bool was_set = false;

    EnvGuard(const std::string& xdg) {
        const char* old = std::getenv("XDG_CONFIG_HOME");
        was_set = old != nullptr;
        if (old) saved_xdg = old;
        setenv("XDG_CONFIG_HOME", xdg.c_str(), 1);
    }
    ~EnvGuard() {
        if (was_set) setenv("XDG_CONFIG_HOME", saved_xdg.c_str(), 1);
        else unsetenv("XDG_CONFIG_HOME");
    }
};

// Stage a fake plugin tree in a temp dir: manifest + executable + chmod.
std::string stage_fake_plugin(const std::string& base) {
    std::string dir = base + "/fake";
    std::filesystem::create_directories(dir);
    std::filesystem::copy_file("tests/plugins/fake_manifest.json", dir + "/manifest.json",
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file("tests/plugins/fake_plugin.py", dir + "/fake_plugin.py",
                               std::filesystem::copy_options::overwrite_existing);
    chmod((dir + "/fake_plugin.py").c_str(), 0755);
    return dir;
}

} // namespace

TEST(plugin_discover_loads_valid_and_flags_invalid) {
    std::string base = "/tmp/amber_plugin_test";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    EnvGuard env(base + "/xdg");

    stage_fake_plugin(base + "/plugins");

    // A plugin with an unsupported protocol version is incompatible.
    std::string bad = base + "/plugins/badproto";
    std::filesystem::create_directories(bad);
    {
        std::ofstream f(bad + "/manifest.json");
        f << R"({"id": "badproto", "name": "Bad", "version": "1.0.0",
                 "protocol_version": 99, "main": "nope"})";
    }

    agent::PluginManager mgr;
    mgr.discover({base + "/plugins"});

    const agent::PluginInfo* fake = mgr.find("fake");
    REQUIRE(fake != nullptr);
    ASSERT_EQ(fake->version, "1.0.0");
    ASSERT(fake->state == agent::PluginState::Disabled);
    ASSERT(fake->error.empty());
    ASSERT(!fake->manifest.completion.is_null());
    ASSERT(fake->manifest.tools.is_array());

    const agent::PluginInfo* badp = mgr.find("badproto");
    REQUIRE(badp != nullptr);
    ASSERT(badp->state == agent::PluginState::Incompatible);
    ASSERT(!badp->error.empty());
}

TEST(plugin_enable_registers_and_runs_tools) {
    std::string base = "/tmp/amber_plugin_test";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    EnvGuard env(base + "/xdg");
    stage_fake_plugin(base + "/plugins");

    agent::PluginManager mgr;
    mgr.discover({base + "/plugins"});
    agent::ToolRegistry reg;

    ASSERT(mgr.enable("fake", reg));
    ASSERT(mgr.find("fake")->state == agent::PluginState::Enabled);

    agent::Tool* echo = reg.find("plugin_fake_echo");
    ASSERT(echo != nullptr);
    agent::Tool* fail = reg.find("plugin_fake_fail");
    ASSERT(fail != nullptr);
    ASSERT_EQ(echo->name(), "plugin_fake_echo");

    auto r = echo->execute({{"text", "hello"}});
    ASSERT(r.ok);
    ASSERT_EQ(r.output, "echo:hello");

    auto rf = fail->execute({});
    ASSERT(!rf.ok);
    ASSERT(rf.output.find("ERROR") != std::string::npos);

    // Disabling unregisters the tools.
    ASSERT(mgr.disable("fake", reg));
    ASSERT(reg.find("plugin_fake_echo") == nullptr);
    ASSERT(mgr.find("fake")->state == agent::PluginState::Disabled);
}

TEST(plugin_state_persists_across_manager_instances) {
    std::string base = "/tmp/amber_plugin_test";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    EnvGuard env(base + "/xdg");
    stage_fake_plugin(base + "/plugins");

    {
        agent::PluginManager mgr;
        mgr.discover({base + "/plugins"});
        agent::ToolRegistry reg;
        ASSERT(mgr.enable("fake", reg));
    }

    agent::PluginManager mgr2;
    mgr2.discover({base + "/plugins"});
    // state.json in the (fake) XDG config dir recorded the enabled flag.
    ASSERT(mgr2.find("fake")->state == agent::PluginState::Enabled);
}

TEST(plugin_install_stages_archive) {
    std::string base = "/tmp/amber_plugin_test";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    EnvGuard env(base + "/xdg");

    // Build a plugin archive: manifest + executable, tar.gz.
    std::string src = base + "/src";
    std::filesystem::create_directories(src);
    std::filesystem::copy_file("tests/plugins/fake_manifest.json", src + "/manifest.json",
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file("tests/plugins/fake_plugin.py", src + "/fake_plugin.py",
                               std::filesystem::copy_options::overwrite_existing);
    chmod((src + "/fake_plugin.py").c_str(), 0755);
    std::string archive = base + "/fake.tar.gz";
    std::string cmd = "tar -czf " + archive + " -C " + src + " .";
    ASSERT(system(cmd.c_str()) == 0);

    agent::PluginManager mgr;
    std::string err = mgr.install(archive);
    ASSERT(err.empty());

    mgr.discover({base + "/xdg/amber/plugins"});
    ASSERT(mgr.find("fake") != nullptr);
    std::string staged = base + "/xdg/amber/plugins/fake/manifest.json";
    ASSERT(std::filesystem::exists(staged));
}

TEST(plugin_advertisement_lists_enabled_tools) {
    std::string base = "/tmp/amber_plugin_test";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    EnvGuard env(base + "/xdg");
    stage_fake_plugin(base + "/plugins");

    agent::PluginManager mgr;
    mgr.discover({base + "/plugins"});
    agent::ToolRegistry reg;
    mgr.enable("fake", reg);

    std::string ad = agent::plugin_tools_advertisement(reg);
    ASSERT(ad.find("## Plugins") != std::string::npos);
    ASSERT(ad.find("plugin_fake_echo") != std::string::npos);
    ASSERT(ad.find("Echo text back.") != std::string::npos);
}

int main() {
    plugin_discover_loads_valid_and_flags_invalid();
    plugin_enable_registers_and_runs_tools();
    plugin_state_persists_across_manager_instances();
    plugin_install_stages_archive();
    plugin_advertisement_lists_enabled_tools();
    if (failed) std::cerr << failed << " FAILED\n";
    std::cout << (failed ? "FAILED" : "ALL PASSED") << " (0 failures)\n";
    return failed ? 1 : 0;
}
