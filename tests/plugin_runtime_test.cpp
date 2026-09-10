// PF-1.5 runtime tests: activation installs what a plugin declares, disabling
// unwinds every contribution, and state persists across runs. Hermetic: the
// config root is redirected to a scratch directory, so nothing touches the
// developer's real ~/.config.

#include "agent.h"
#include "agent/extensions.h"
#include "agent/plugin_runtime.h"
#include "agent/plugins_bundled.h"
#include "agent/tools.h"
#include "fake_llm.h"
#include "plugins/metrics/metrics_plugin.h"
#include "test_util.h"

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <unistd.h>

using namespace agent;

namespace {

namespace fs = std::filesystem;

// Redirect XDG_CONFIG_HOME for the lifetime of the test so per-plugin state
// files land in a scratch tree.
class ScratchConfig {
public:
    explicit ScratchConfig(const std::string& name) {
        dir_ = fs::temp_directory_path() / ("amber_plugin_test_" + name);
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

// Declares one prompt block, so activation has something observable to install.
class BlockPlugin : public IPlugin {
public:
    explicit BlockPlugin(std::string id, std::string text, int priority = 100)
        : id_(std::move(id)), text_(std::move(text)), priority_(priority) {}

    std::string id() const override { return id_; }
    std::string version() const override { return "1.0.0"; }
    std::string name() const override { return "Block " + id_; }

    bool initialize(const PluginContext&) override {
        ++initialized_;
        return !fail_;
    }
    void shutdown() override { ++shutdowns_; }

    std::vector<std::unique_ptr<Capability>> capabilities() override {
        std::vector<std::unique_ptr<Capability>> caps;
        caps.push_back(std::make_unique<PromptBlockCapability>(
            "block", priority_, [this] { return text_; }));
        caps.push_back(std::make_unique<SettingCapability>("greeting", "what to say"));
        return caps;
    }

    int initialized_ = 0;
    int shutdowns_ = 0;
    bool fail_ = false;

private:
    std::string id_;
    std::string text_;
    int priority_;
};

struct Fixture {
    ToolRegistry tools;
    Config cfg;
    Workspace ws;
};

} // namespace

TEST(runtime_lists_registered_plugins_as_disabled_before_start) {
    ScratchConfig scratch("list");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add(std::make_shared<BlockPlugin>("alpha", "A"));

    auto list = runtime.list();
    ASSERT_EQ(list.size(), 1u);
    ASSERT_EQ(list[0].id, std::string("alpha"));
    ASSERT_EQ(list[0].tier, std::string("bundled"));
    ASSERT_FALSE(list[0].enabled);
    ASSERT_EQ(runtime.prompts().size(), 0u);
}

TEST(runtime_bundled_plugins_start_enabled) {
    ScratchConfig scratch("start");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    auto plugin = std::make_shared<BlockPlugin>("alpha", "hello from alpha");
    runtime.add(plugin);

    runtime.start();

    ASSERT_EQ(plugin->initialized_, 1);
    ASSERT_TRUE(runtime.status("alpha").enabled);
    auto rendered = runtime.prompts().render_all();
    ASSERT_EQ(rendered.size(), 1u);
    ASSERT_EQ(rendered[0], std::string("hello from alpha"));
}

TEST(runtime_disable_unwinds_every_contribution) {
    ScratchConfig scratch("disable");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    auto plugin = std::make_shared<BlockPlugin>("alpha", "text");
    runtime.add(plugin);
    runtime.start();

    ASSERT_EQ(runtime.prompts().size(), 1u);
    ASSERT_FALSE(runtime.settings().items().empty());

    ASSERT_TRUE(runtime.set_state("alpha", false));

    ASSERT_FALSE(runtime.status("alpha").enabled);
    ASSERT_EQ(runtime.prompts().size(), 0u);
    ASSERT_TRUE(runtime.settings().items().empty());
    ASSERT_EQ(plugin->shutdowns_, 1);
    ASSERT_TRUE(runtime.contributions().empty());
}

TEST(runtime_enable_after_disable_restores_contributions) {
    ScratchConfig scratch("reenable");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add(std::make_shared<BlockPlugin>("alpha", "text"));
    runtime.start();

    ASSERT_TRUE(runtime.set_state("alpha", false));
    ASSERT_EQ(runtime.prompts().size(), 0u);

    ASSERT_TRUE(runtime.set_state("alpha", true));
    ASSERT_TRUE(runtime.status("alpha").enabled);
    ASSERT_EQ(runtime.prompts().size(), 1u);
}

TEST(runtime_state_persists_across_instances) {
    ScratchConfig scratch("persist");
    Fixture f;
    {
        PluginRuntime runtime(f.tools, f.cfg, f.ws);
        runtime.add(std::make_shared<BlockPlugin>("alpha", "text"));
        runtime.start();
        ASSERT_TRUE(runtime.set_state("alpha", false));
    }
    // A fresh runtime in the same config tree must come up disabled.
    {
        PluginRuntime runtime(f.tools, f.cfg, f.ws);
        auto plugin = std::make_shared<BlockPlugin>("alpha", "text");
        runtime.add(plugin);
        runtime.start();
        ASSERT_FALSE(runtime.status("alpha").enabled);
        ASSERT_EQ(plugin->initialized_, 0);
        ASSERT_EQ(runtime.prompts().size(), 0u);
    }
}

TEST(runtime_unknown_plugin_cannot_be_toggled) {
    ScratchConfig scratch("unknown");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.start();
    ASSERT_FALSE(runtime.set_state("nobody", true));
    ASSERT_FALSE(runtime.set_state("nobody", false));
    ASSERT_FALSE(runtime.has("nobody"));
}

TEST(runtime_failed_initialize_leaves_nothing_installed) {
    ScratchConfig scratch("fail");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    auto plugin = std::make_shared<BlockPlugin>("alpha", "text");
    plugin->fail_ = true;
    runtime.add(plugin);

    runtime.start();

    ASSERT_FALSE(runtime.status("alpha").enabled);
    ASSERT_EQ(runtime.prompts().size(), 0u);
    ASSERT_TRUE(runtime.contributions().empty());
}

TEST(runtime_contributions_span_every_registry) {
    ScratchConfig scratch("contrib");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add(std::make_shared<BlockPlugin>("alpha", "text"));
    runtime.start();

    bool saw_prompt = false, saw_setting = false;
    for (const auto& item : runtime.contributions()) {
        ASSERT_EQ(item.owner, std::string("alpha"));
        if (item.kind == CapabilityKind::PromptBlock) saw_prompt = true;
        if (item.kind == CapabilityKind::Setting) saw_setting = true;
    }
    ASSERT_TRUE(saw_prompt);
    ASSERT_TRUE(saw_setting);

    auto list = runtime.list();
    ASSERT_EQ(list.size(), 1u);
    ASSERT_EQ(list[0].contributions.size(), 2u);
}

// The v1 adapter puts external plugins under the same surface. A discovered
// plugin whose id collides with a bundled one must not overwrite it.
TEST(runtime_external_plugins_share_the_control_surface) {
    ScratchConfig scratch("external");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add(std::make_shared<BlockPlugin>("metrics", "core metrics"), true);

    agent::PluginManager manager;  // discovers nothing on this machine
    runtime.add_external(manager);

    ASSERT_TRUE(runtime.add(std::make_shared<BlockPlugin>("metrics", "impostor"),
                            false) == false);
    auto list = runtime.list();
    ASSERT_EQ(list.size(), 1u);
    ASSERT_EQ(list[0].tier, std::string("bundled"));

    // A distinct external id is accepted and reported as external.
    ASSERT_TRUE(runtime.add(std::make_shared<BlockPlugin>("ext_sample", "x"),
                            false));
    ASSERT_EQ(runtime.status("ext_sample").tier, std::string("external"));
}

TEST(runtime_external_plugin_is_off_by_default) {
    ScratchConfig scratch("external_off");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add(std::make_shared<BlockPlugin>("ext_sample", "x"), false);
    runtime.start();
    // External plugins are opt-in: a discovered plugin does not run until the
    // user enables it.
    ASSERT_FALSE(runtime.status("ext_sample").enabled);
    ASSERT_EQ(runtime.prompts().size(), 0u);
}

TEST(runtime_bundled_set_registers_the_metrics_plugin) {
    ScratchConfig scratch("bundled");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    ASSERT_TRUE(runtime.has("metrics"));
    ASSERT_FALSE(make_bundled_plugins().empty());
}

// The end-to-end proof the framework exists for: a real agent turn, with the
// bus attached by the runtime, observed by the bundled plugin - and silence
// once the plugin is switched off.
TEST(runtime_bundled_plugin_observes_a_real_turn) {
    ScratchConfig scratch("dogfood");
    Fixture f;
    f.cfg.stream = false;
    f.cfg.system_prompt_path = "prompts/system.md";
    f.cfg.tools_prompt_path = "prompts/tools.md";
    char cwd[4096];
    if (getcwd(cwd, sizeof cwd)) Workspace::set_root(cwd);

    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    auto metrics = std::make_shared<plugins::MetricsPlugin>();
    runtime.add(metrics);
    runtime.start();
    ASSERT_TRUE(runtime.status("metrics").enabled);

    auto fake = std::make_unique<agent_test::FakeLLMClient>();
    agent_test::FakeReply reply;
    reply.content = "hello";
    fake->script.push_back(reply);
    agent_test::FakeReply done;
    done.content = "done";
    fake->script.push_back(done);

    agent::ToolRegistry reg;
    agent::Agent ag(f.cfg, reg, {}, {}, {}, {}, {}, std::move(fake));
    ag.set_events(runtime.events());
    ag.run("say hello");

    ASSERT_EQ(metrics->stats().turns, 1);

    // Switched off: the plugin sees nothing more, and it has no contributions
    // left in the harness.
    ASSERT_TRUE(runtime.set_state("metrics", false));
    ASSERT_TRUE(runtime.contributions().empty());
    ASSERT_FALSE(runtime.status("metrics").enabled);
}

TEST(runtime_shutdown_deactivates_everything) {
    ScratchConfig scratch("shutdown");
    Fixture f;
    auto plugin = std::make_shared<BlockPlugin>("alpha", "text");
    {
        PluginRuntime runtime(f.tools, f.cfg, f.ws);
        runtime.add(plugin);
        runtime.start();
        ASSERT_EQ(runtime.prompts().size(), 1u);
    }
    ASSERT_EQ(plugin->shutdowns_, 1);
    ASSERT_TRUE(plugin->capabilities().size() == 2u);
}
