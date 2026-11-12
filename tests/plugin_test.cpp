#include "agent/plugin.h"
#include "agent/tools.h"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <sys/stat.h>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include "tests/minitest.h"
#include "tests/test_wait.h"

using agent::test::elapsed_ms;
using agent::test::wait_until;

#define REQUIRE(cond)                                                                              \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::cerr << "FAIL: " << #cond << "\n";                                                \
            failed++;                                                                              \
            return;                                                                                \
        }                                                                                          \
    } while (0)

namespace {

struct EnvGuard {
    std::string saved_xdg;
    std::string saved_data;
    bool was_set = false;
    bool data_was_set = false;
    bool touched_data = false;

    EnvGuard(const std::string& xdg, const std::string& data = "") {
        const char* old = std::getenv("XDG_CONFIG_HOME");
        was_set = old != nullptr;
        if (old)
            saved_xdg = old;
        setenv("XDG_CONFIG_HOME", xdg.c_str(), 1);
        touched_data = !data.empty();
        if (touched_data) {
            const char* old_data = std::getenv("XDG_DATA_HOME");
            data_was_set = old_data != nullptr;
            if (old_data)
                saved_data = old_data;
            setenv("XDG_DATA_HOME", data.c_str(), 1);
        }
    }
    ~EnvGuard() {
        if (was_set)
            setenv("XDG_CONFIG_HOME", saved_xdg.c_str(), 1);
        else
            unsetenv("XDG_CONFIG_HOME");
        if (touched_data) {
            if (data_was_set)
                setenv("XDG_DATA_HOME", saved_data.c_str(), 1);
            else
                unsetenv("XDG_DATA_HOME");
        }
    }
};

// Each scenario is named in the output, so a failure in a repeated run can be
// attributed to a test instead of to the binary.
int run_one(const char* name, void (*fn)()) {
    const int before = failed;
    std::cout << "[ RUN  ] " << name << "\n";
    fn();
    if (failed == before) {
        std::cout << "[ PASS ] " << name << "\n";
        return 0;
    }
    std::cout << "[ FAIL ] " << name << "\n";
    return 1;
}

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

// Stage one of the misbehaving fixtures with a single `call` tool.
std::string stage_test_plugin(const std::string& base, const std::string& id,
                              const std::string& script) {
    const std::string dir = base + "/plugins/" + id;
    std::filesystem::create_directories(dir);
    std::filesystem::copy_file("tests/plugins/" + script, dir + "/" + script,
                               std::filesystem::copy_options::overwrite_existing);
    chmod((dir + "/" + script).c_str(), 0755);
    std::ofstream manifest(dir + "/manifest.json");
    manifest << R"({"id": ")" << id << R"(", "name": ")" << id
             << R"(", "version": "1.0.0", "protocol_version": 1, "main": ")" << script
             << R"(", "tools": [{"name": "call", "description": "test", "schema": {}}]})";
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

    auto echo = reg.find("plugin_fake_echo");
    ASSERT(echo != nullptr);
    auto fail = reg.find("plugin_fake_fail");
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

TEST(plugin_disable_cancels_a_hung_call) {
    std::string base = "/tmp/amber_plugin_test";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    EnvGuard env(base + "/xdg");
    const std::string marker = base + "/call.marker";
    setenv("AMBER_PLUGIN_TEST_MARKER", marker.c_str(), 1);
    stage_test_plugin(base, "hang", "hang_plugin.py");

    agent::PluginManager mgr;
    mgr.discover({base + "/plugins"});
    agent::ToolRegistry reg;
    ASSERT(mgr.enable("hang", reg));
    auto call = reg.find("plugin_hang_call");
    REQUIRE(call != nullptr);

    agent::ToolResult result;
    std::thread worker([&] { result = call->execute({}); });
    REQUIRE(wait_until([&] { return std::filesystem::exists(marker); }));

    // The call is parked inside its 60 s read window; disabling must cancel it
    // and kill the process instead of waiting for that deadline.
    // Bounded, not fast: the regression is a 60 s wait, so a generous bound
    // keeps a loaded macOS runner green while still catching that regression.
    const auto started = std::chrono::steady_clock::now();
    ASSERT(mgr.disable("hang", reg));
    ASSERT(elapsed_ms(started) < 20000);
    worker.join();

    ASSERT(!result.ok);
    ASSERT(result.error.find("cancelled") != std::string::npos ||
           result.error.find("exited") != std::string::npos);
    ASSERT(mgr.find("hang")->state == agent::PluginState::Disabled);
    unsetenv("AMBER_PLUGIN_TEST_MARKER");
}

TEST(plugin_disable_kills_a_plugin_that_ignores_shutdown) {
    std::string base = "/tmp/amber_plugin_test";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    EnvGuard env(base + "/xdg");
    stage_test_plugin(base, "slowexit", "slow_shutdown_plugin.py");

    agent::PluginManager mgr;
    mgr.discover({base + "/plugins"});
    agent::ToolRegistry reg;
    ASSERT(mgr.enable("slowexit", reg));
    auto call = reg.find("plugin_slowexit_call");
    REQUIRE(call != nullptr);
    ASSERT(call->execute({}).ok);

    // The process ignores the shutdown notification; teardown gets its bounded
    // grace window and then SIGKILLs it.
    const auto started = std::chrono::steady_clock::now();
    ASSERT(mgr.disable("slowexit", reg));
    ASSERT(elapsed_ms(started) < 20000);
    ASSERT(mgr.find("slowexit")->state == agent::PluginState::Disabled);
}

TEST(plugin_oversized_initialize_line_fails_closed) {
    std::string base = "/tmp/amber_plugin_test";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    EnvGuard env(base + "/xdg");
    setenv("AMBER_PLUGIN_TEST_OVERSIZE", "initialize", 1);
    stage_test_plugin(base, "biginit", "oversized_plugin.py");

    agent::PluginManager mgr;
    mgr.discover({base + "/plugins"});
    agent::ToolRegistry reg;
    // Activation is lazy, so the handshake happens on the first call.
    ASSERT(mgr.enable("biginit", reg));
    auto call = reg.find("plugin_biginit_call");
    REQUIRE(call != nullptr);
    const agent::ToolResult result = call->execute({});
    ASSERT(!result.ok);
    ASSERT(result.error.find("64 KiB") != std::string::npos);
    const agent::PluginInfo* info = mgr.find("biginit");
    REQUIRE(info != nullptr);
    ASSERT(info->error.find("64 KiB") != std::string::npos);
    unsetenv("AMBER_PLUGIN_TEST_OVERSIZE");
}

TEST(plugin_oversized_call_reply_fails_closed_and_respawns) {
    std::string base = "/tmp/amber_plugin_test";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    EnvGuard env(base + "/xdg");
    unsetenv("AMBER_PLUGIN_TEST_OVERSIZE");
    stage_test_plugin(base, "bigreply", "oversized_plugin.py");

    agent::PluginManager mgr;
    mgr.discover({base + "/plugins"});
    agent::ToolRegistry reg;
    ASSERT(mgr.enable("bigreply", reg));
    auto call = reg.find("plugin_bigreply_call");
    REQUIRE(call != nullptr);

    const agent::ToolResult first = call->execute({});
    ASSERT(!first.ok);
    ASSERT(first.error.find("64 KiB") != std::string::npos);

    // The poisoned stream is dropped, so the next call starts a fresh process
    // and fails at the cap again rather than consuming a stale reply.
    const auto started = std::chrono::steady_clock::now();
    const agent::ToolResult second = call->execute({});
    ASSERT(!second.ok);
    ASSERT(second.error.find("64 KiB") != std::string::npos);
    ASSERT(elapsed_ms(started) < 20000);
    ASSERT(mgr.disable("bigreply", reg));
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

// ---------------------------------------------------------------------------
// SEC-05: plugin manifest path safety (Red tests). The manifest "main"
// field must not allow path traversal (../) or absolute escapes, and
// symlinked entry points must be rejected.
// ---------------------------------------------------------------------------

TEST(sec05_manifest_rejects_parent_escape_main) {
    std::string base = "/tmp/amber_sec05_plugin";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    EnvGuard env(base + "/xdg");

    std::string dir = base + "/plugins/escape";
    std::filesystem::create_directories(dir);
    // A manifest with main="../../bin/sh" — a path traversal escape.
    {
        std::ofstream f(dir + "/manifest.json");
        f << R"({"id": "escape", "name": "Escape", "version": "1.0.0",
                 "protocol_version": 1, "main": "../../bin/sh"})";
    }
    // Create a fake executable at the escaped path so is_executable would pass
    // if the path check were absent.
    std::string outside = base + "/bin";
    std::filesystem::create_directories(outside);
    {
        std::ofstream f(outside + "/sh");
        f << "#!/bin/sh\nexit 0\n";
    }
    chmod((outside + "/sh").c_str(), 0755);

    agent::PluginManager mgr;
    mgr.discover({base + "/plugins"});
    const agent::PluginInfo* info = mgr.find("escape");
    REQUIRE(info != nullptr);
    // The manifest must be rejected for path traversal, not loaded.
    ASSERT(info->state == agent::PluginState::Incompatible);
    ASSERT(!info->error.empty());
    std::filesystem::remove_all(base);
}

TEST(sec05_manifest_rejects_absolute_main) {
    std::string base = "/tmp/amber_sec05_plugin";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    EnvGuard env(base + "/xdg");

    std::string dir = base + "/plugins/abs";
    std::filesystem::create_directories(dir);
    // A manifest with main="/bin/sh" — an absolute path escape.
    {
        std::ofstream f(dir + "/manifest.json");
        f << R"({"id": "abs", "name": "Abs", "version": "1.0.0",
                 "protocol_version": 1, "main": "/bin/sh"})";
    }

    agent::PluginManager mgr;
    mgr.discover({base + "/plugins"});
    const agent::PluginInfo* info = mgr.find("abs");
    REQUIRE(info != nullptr);
    ASSERT(info->state == agent::PluginState::Incompatible);
    ASSERT(!info->error.empty());
    std::filesystem::remove_all(base);
}

TEST(sec05_manifest_rejects_symlink_main) {
    std::string base = "/tmp/amber_sec05_plugin";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    EnvGuard env(base + "/xdg");

    std::string dir = base + "/plugins/sym";
    std::filesystem::create_directories(dir);
    // A real executable outside the plugin dir.
    std::string outside = base + "/outside.sh";
    {
        std::ofstream f(outside);
        f << "#!/bin/sh\necho pwned\n";
    }
    chmod(outside.c_str(), 0755);
    // A symlink inside the plugin dir pointing to the outside executable.
    std::filesystem::create_symlink(outside, dir + "/link.sh");
    {
        std::ofstream f(dir + "/manifest.json");
        f << R"({"id": "sym", "name": "Sym", "version": "1.0.0",
                 "protocol_version": 1, "main": "link.sh"})";
    }

    agent::PluginManager mgr;
    mgr.discover({base + "/plugins"});
    const agent::PluginInfo* info = mgr.find("sym");
    REQUIRE(info != nullptr);
    // A symlinked main pointing outside the plugin dir must be rejected.
    ASSERT(info->state == agent::PluginState::Incompatible);
    ASSERT(!info->error.empty());
    std::filesystem::remove_all(base);
}

// The XDG data root is where a user installs external plugins (and where a
// relocatable prefix can expose them); discovery must scan it.
TEST(plugin_discovers_the_xdg_data_root) {
    std::string base = "/tmp/amber_plugin_data_root";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    EnvGuard env(base + "/xdg-config", base + "/xdg-data");
    stage_fake_plugin(base + "/xdg-data/amber/plugins");

    agent::PluginManager mgr;
    mgr.discover();
    const agent::PluginInfo* info = mgr.find("fake");
    REQUIRE(info != nullptr);
    ASSERT(info->state == agent::PluginState::Disabled);
    std::filesystem::remove_all(base);
}

// The bundled sysinfo plugin (built by make) must load and report real
// facts through the same protocol as any other plugin.
TEST(sysinfo_plugin_reports_host_facts) {
    std::string base = "/tmp/amber_plugin_test";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    EnvGuard env(base + "/xdg");

    std::string dir = base + "/plugins/sysinfo";
    std::filesystem::create_directories(dir);
    REQUIRE(std::filesystem::exists("tools/plugins/sysinfo/sysinfo-plugin"));
    std::filesystem::copy_file("tools/plugins/sysinfo/manifest.json", dir + "/manifest.json",
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file("tools/plugins/sysinfo/sysinfo-plugin", dir + "/sysinfo-plugin",
                               std::filesystem::copy_options::overwrite_existing);
    chmod((dir + "/sysinfo-plugin").c_str(), 0755);

    agent::PluginManager mgr;
    mgr.discover({base + "/plugins"});
    agent::ToolRegistry reg;
    REQUIRE(mgr.enable("sysinfo", reg));

    auto mem = reg.find("plugin_sysinfo_mem");
    REQUIRE(mem != nullptr);
    auto r = mem->execute({});
    ASSERT(r.ok);
    ASSERT(r.output.find("MB") != std::string::npos);

    auto cpu = reg.find("plugin_sysinfo_cpu");
    REQUIRE(cpu != nullptr);
    auto rc = cpu->execute({});
    ASSERT(rc.ok);
    ASSERT(rc.output.find("load") != std::string::npos);

    auto net = reg.find("plugin_sysinfo_net");
    REQUIRE(net != nullptr);
    auto rn = net->execute({});
    ASSERT(rn.ok);
    ASSERT(!rn.output.empty());

    ASSERT(mgr.disable("sysinfo", reg));
}

// The CDP plugin must load and answer the protocol even when no browser is
// reachable; with the framebuffer Chrome up (localhost:9222) it reports real
// targets. Either outcome is a valid, non-crashing response.
TEST(cdp_plugin_protocol_roundtrip) {
    std::string base = "/tmp/amber_plugin_test";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    EnvGuard env(base + "/xdg");

    std::string dir = base + "/plugins/cdp";
    std::filesystem::create_directories(dir);
    REQUIRE(std::filesystem::exists("tools/plugins/cdp/cdp-plugin"));
    std::filesystem::copy_file("tools/plugins/cdp/manifest.json", dir + "/manifest.json",
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file("tools/plugins/cdp/cdp-plugin", dir + "/cdp-plugin",
                               std::filesystem::copy_options::overwrite_existing);
    chmod((dir + "/cdp-plugin").c_str(), 0755);

    agent::PluginManager mgr;
    mgr.discover({base + "/plugins"});
    agent::ToolRegistry reg;
    REQUIRE(mgr.enable("cdp", reg));

    auto targets = reg.find("plugin_cdp_list_targets");
    REQUIRE(targets != nullptr);
    auto r = targets->execute({});
    // Either a live list (Chrome running) or a clean connection error.
    if (!r.ok) {
        bool clean_err = r.output.find("CDP") != std::string::npos ||
                         r.output.find("connect") != std::string::npos;
        ASSERT(clean_err);
    }
    ASSERT(mgr.disable("cdp", reg));
}

// The v2 runtime keeps per-plugin state at ~/.config/amber/plugins/<id>/plugin.conf,
// the same root external plugins install into. A directory holding only state is
// not an install: it must not surface as an incompatible ghost of a real plugin.
TEST(plugin_discover_ignores_state_dirs) {
    std::string base = "/tmp/amber_plugin_test";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    EnvGuard env(base + "/xdg");
    stage_fake_plugin(base + "/plugins");

    std::string state = base + "/plugins/clock";
    std::filesystem::create_directories(state);
    {
        std::ofstream f(state + "/plugin.conf");
        f << "# amber plugin state: clock\nenabled=1\n";
    }

    agent::PluginManager mgr;
    mgr.discover({base + "/plugins"});

    ASSERT(mgr.find("clock") == nullptr);
    ASSERT(mgr.find("fake") != nullptr);
}

// A response line over the 64 KiB cap fails closed instead of growing the
// parser's buffer without bound.
TEST(plugin_response_line_is_capped) {
    std::string base = "/tmp/amber_plugin_test";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base);
    EnvGuard env(base + "/xdg");
    std::string dir = base + "/plugins/big";
    std::filesystem::create_directories(dir);
    {
        std::ofstream f(dir + "/manifest.json");
        f << R"({"id":"big","name":"Big","version":"1.0.0","protocol_version":1,
                 "main":"big.py","tools":[{"name":"huge","description":"huge",
                 "schema":{"type":"object","properties":{}}}]})";
    }
    {
        std::ofstream f(dir + "/big.py");
        f << "#!/usr/bin/env python3\n"
             "import json, sys\n"
             "for line in sys.stdin:\n"
             "    m = json.loads(line)\n"
             "    if m.get('method') == 'initialize':\n"
             "        print(json.dumps({'id': m['id'], 'result': "
             "{'protocol_version': 1, 'ok': True}}), flush=True)\n"
             "    elif m.get('method') == 'tool.call':\n"
             "        print(json.dumps({'id': m['id'], 'result': "
             "{'ok': True, 'output': 'x' * 70000}}), flush=True)\n"
             "    elif m.get('method') == 'shutdown':\n"
             "        sys.exit(0)\n";
    }
    chmod((dir + "/big.py").c_str(), 0755);

    agent::PluginManager mgr;
    mgr.discover({base + "/plugins"});
    agent::ToolRegistry reg;
    ASSERT(mgr.enable("big", reg));
    auto tool = reg.find("plugin_big_huge");
    REQUIRE(tool != nullptr);
    auto r = tool->execute({});
    ASSERT(!r.ok);
    ASSERT(r.error.find("64 KiB") != std::string::npos);
    ASSERT(mgr.disable("big", reg));
}

#define RUN_TEST(fn) run_one(#fn, fn)

int main() {
    RUN_TEST(plugin_discover_loads_valid_and_flags_invalid);
    RUN_TEST(plugin_discover_ignores_state_dirs);
    RUN_TEST(plugin_enable_registers_and_runs_tools);
    RUN_TEST(plugin_disable_cancels_a_hung_call);
    RUN_TEST(plugin_disable_kills_a_plugin_that_ignores_shutdown);
    RUN_TEST(plugin_oversized_initialize_line_fails_closed);
    RUN_TEST(plugin_oversized_call_reply_fails_closed_and_respawns);
    RUN_TEST(plugin_state_persists_across_manager_instances);
    RUN_TEST(plugin_install_stages_archive);
    RUN_TEST(plugin_advertisement_lists_enabled_tools);
    RUN_TEST(plugin_response_line_is_capped);
    RUN_TEST(sec05_manifest_rejects_parent_escape_main);
    RUN_TEST(sec05_manifest_rejects_absolute_main);
    RUN_TEST(sec05_manifest_rejects_symlink_main);
    RUN_TEST(sysinfo_plugin_reports_host_facts);
    RUN_TEST(cdp_plugin_protocol_roundtrip);
    if (failed)
        std::cerr << failed << " FAILED\n";
    std::cout << (failed ? "FAILED" : "ALL PASSED") << " (" << failed << " failures)\n";
    return failed ? 1 : 0;
}
