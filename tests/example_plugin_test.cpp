// Compiles and exercises the reference plugin in examples/plugin_hello/.
//
// This test is the guard for the developer guide's "core plugin anatomy": it
// proves the documented shape builds, that a declared capability installs into
// the registry, and that disable unwinds it. If this stops compiling, the
// guide's example is wrong.

#include "agent/extensions.h"
#include "agent/plugin_runtime.h"
#include "examples/plugin_hello/hello_plugin.h"
#include "test_util.h"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>

#include <nlohmann/json.hpp>

using namespace agent;

namespace {

namespace fs = std::filesystem;

// Keep per-plugin state out of the developer's real ~/.config.
class ScratchConfig {
public:
    explicit ScratchConfig(const std::string& name)
        : dir_(fs::temp_directory_path() / ("amber_example_test_" + name)) {
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        setenv("XDG_CONFIG_HOME", dir_.c_str(), 1);
    }
    ~ScratchConfig() {
        unsetenv("XDG_CONFIG_HOME");
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    ScratchConfig(const ScratchConfig&) = delete;
    ScratchConfig& operator=(const ScratchConfig&) = delete;

private:
    fs::path dir_;
};

struct Fixture {
    ToolRegistry tools;
    Config cfg;
    Workspace ws;
};

} // namespace

TEST(example_hello_plugin_contributes_and_unwinds_a_tool) {
    ScratchConfig scratch("hello");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add(std::make_shared<examples::HelloPlugin>());

    // Registered but not started: nothing is installed yet.
    ASSERT_FALSE((bool)f.tools.find("greet"));

    runtime.start();
    ASSERT_TRUE(runtime.status("hello").enabled);

    auto tool = f.tools.find("greet");
    ASSERT_TRUE((bool)tool);
    ASSERT_EQ(tool->execute(json{{"name", "amber"}}).output, std::string("Hello, amber!"));

    // Disable unwinds the ledger: the tool is gone.
    ASSERT_TRUE(runtime.set_state("hello", false));
    ASSERT_FALSE((bool)f.tools.find("greet"));
}
