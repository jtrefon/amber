// Search backends as plugin capabilities (spec §4).
//
// A backend is a contribution: a mode name and a factory, installed under the
// contributing plugin's owner and unwound by the ledger. The search tool
// resolves modes through a provider, so a mode whose plugin is switched off
// reports which plugin to enable instead of reading as unknown, and the
// tool's schema lists whatever is enabled right now. Hermetic: no process, no
// network, no terminal.

#include "agent/extensions.h"
#include "agent/plugin_core.h"
#include "agent/plugin_runtime.h"
#include "agent/search_backend.h"
#include "agent/tools.h"
#include "test_util.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>

using namespace agent;

namespace {

class FakeBackend : public SearchBackend {
public:
    explicit FakeBackend(std::string name) : name_(std::move(name)) {}

    std::string name() const noexcept override { return name_; }

    std::vector<SearchHit> search(const std::string&, const std::string&, const std::string&, long,
                                  const std::vector<std::string>&) const override {
        return {SearchHit{"fake.cpp", 1, "hit from " + name_, 1.0}};
    }

private:
    std::string name_;
};

// Services wired to nothing else: registry-level tests need no runtime.
struct RegistryFixture {
    ToolRegistry tools;
    PromptRegistry prompts;
    StatusRegistry status;
    PanelRegistry panels;
    WalletRegistry wallets;
    EventBus events;
    CommandRegistry commands;
    std::shared_ptr<SearchBackendRegistry> backends = std::make_shared<SearchBackendRegistry>();
    PluginServices services;

    RegistryFixture()
        : services(tools, prompts, status, panels, wallets, backends, events, commands) {
        services.set_owner("test");
    }
};

// Keeps per-plugin state out of the developer's real ~/.config.
class ScratchConfig {
public:
    explicit ScratchConfig(const std::string& name)
        : dir_(std::filesystem::temp_directory_path() / ("amber_search_backends_" + name)) {
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
        setenv("XDG_CONFIG_HOME", dir_.c_str(), 1);
    }
    ~ScratchConfig() {
        unsetenv("XDG_CONFIG_HOME");
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    ScratchConfig(const ScratchConfig&) = delete;
    ScratchConfig& operator=(const ScratchConfig&) = delete;

private:
    std::filesystem::path dir_;
};

// A plugin that contributes one backend, for the runtime-level tests.
class BackendPlugin : public IPlugin {
public:
    std::string id() const override { return "fake_backend"; }
    std::string version() const override { return "0.0.1"; }
    std::string name() const override { return "Fake backend"; }
    bool initialize(const PluginContext&) override { return true; }
    void shutdown() override {}

    std::vector<std::unique_ptr<Capability>> capabilities() override {
        std::vector<std::unique_ptr<Capability>> caps;
        caps.push_back(std::make_unique<SearchBackendCapability>(
            "fake", [] { return std::make_unique<FakeBackend>("fake"); }));
        return caps;
    }
};

} // namespace

TEST(search_backend_capability_installs_and_unwinds) {
    RegistryFixture f;
    SearchBackendCapability cap("fake", [] { return std::make_unique<FakeBackend>("fake"); });

    InstallResult r = cap.install(f.services);
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.contribution.kind == CapabilityKind::SearchBackend);
    ASSERT_EQ(f.backends->size(), 1u);

    auto backend = f.backends->create("fake");
    ASSERT_TRUE((bool)backend);
    ASSERT_EQ(backend->name(), std::string("fake"));

    r.contribution.remove();
    ASSERT_EQ(f.backends->size(), 0u);
    ASSERT_FALSE((bool)f.backends->create("fake"));
}

// Two plugins, one mode: the later registration wins, and the first plugin's
// unwind must not take the winner's backend away (the ledger is identity-scoped,
// as it is for tools).
TEST(search_backend_removal_is_owner_scoped) {
    RegistryFixture f;
    auto first =
        f.backends->add("alpha", "fake", [] { return std::make_unique<FakeBackend>("alpha"); });
    auto second =
        f.backends->add("beta", "fake", [] { return std::make_unique<FakeBackend>("beta"); });

    first.remove();
    auto backend = f.backends->create("fake");
    ASSERT_TRUE((bool)backend);
    ASSERT_EQ(backend->name(), std::string("beta"));

    second.remove();
    ASSERT_FALSE((bool)f.backends->create("fake"));
}

// A plugin that ships switched off still declares its mode: resolving it must
// name the plugin and the command that enables it, never read as "unknown"
// (the same rule provider flavors follow, D19).
TEST(search_backend_declared_but_disabled_is_known) {
    RegistryFixture f;
    f.backends->declare("fake", "some_plugin");

    ASSERT_FALSE((bool)f.backends->create("fake"));
    const std::string reason = f.backends->unavailable_reason("fake");
    ASSERT(reason.find("some_plugin") != std::string::npos);
    ASSERT(reason.find("/set plugin some_plugin on") != std::string::npos);

    // Installing clears the mark, removing marks it again.
    auto c = f.backends->add("some_plugin", "fake",
                             [] { return std::make_unique<FakeBackend>("fake"); });
    ASSERT_TRUE(f.backends->unavailable_reason("fake").empty());
    c.remove();
    ASSERT(f.backends->unavailable_reason("fake").find("some_plugin") != std::string::npos);
}

// A mode nobody provides is unknown, and that is distinguishable from disabled.
TEST(search_backend_unknown_mode_has_no_reason) {
    RegistryFixture f;
    ASSERT_TRUE(f.backends->unavailable_reason("nope").empty());
    ASSERT_FALSE((bool)f.backends->create("nope"));
}

TEST(search_tool_resolves_a_contributed_backend) {
    RegistryFixture f;
    auto c = f.backends->add("third_party", "fake",
                             [] { return std::make_unique<FakeBackend>("fake"); });
    auto tool = make_search_tool(make_search_backend_provider(f.backends));

    auto r = tool->execute({{"pattern", "anything"}, {"mode", "fake"}});
    ASSERT_TRUE(r.ok);
    ASSERT(r.output.find("[fake]") != std::string::npos);
    ASSERT(r.output.find("hit from fake") != std::string::npos);

    c.remove();
    auto after = tool->execute({{"pattern", "anything"}, {"mode", "fake"}});
    ASSERT_FALSE(after.ok); // the provider reads the live table, not a snapshot
}

TEST(search_tool_unknown_mode_lists_enabled_modes) {
    RegistryFixture f;
    f.backends->add("alpha", "fake", [] { return std::make_unique<FakeBackend>("fake"); });
    auto tool = make_search_tool(make_search_backend_provider(f.backends));

    auto r = tool->execute({{"pattern", "x"}, {"mode", "nope"}});
    ASSERT_FALSE(r.ok);
    ASSERT(r.error.find("nope") != std::string::npos);
    ASSERT(r.error.find("fake") != std::string::npos); // what is enabled
}

TEST(search_tool_disabled_mode_names_its_plugin) {
    RegistryFixture f;
    auto c = f.backends->add("some_plugin", "fake",
                             [] { return std::make_unique<FakeBackend>("fake"); });
    c.remove(); // known, but switched off
    auto tool = make_search_tool(make_search_backend_provider(f.backends));

    auto r = tool->execute({{"pattern", "x"}, {"mode", "fake"}});
    ASSERT_FALSE(r.ok);
    ASSERT(r.error.find("some_plugin") != std::string::npos);
    ASSERT(r.error.find("/set plugin some_plugin on") != std::string::npos);
}

TEST(search_tool_with_no_backends_says_so) {
    RegistryFixture f;
    auto tool = make_search_tool(make_search_backend_provider(f.backends));

    auto r = tool->execute({{"pattern", "x"}, {"mode", "grep"}});
    ASSERT_FALSE(r.ok);
    ASSERT(r.error.find("no search backend is enabled") != std::string::npos);
}

// The model-facing mode list is rebuilt with the schema, so disabling a backend
// updates the documentation in the same action.
TEST(search_tool_schema_lists_live_modes) {
    RegistryFixture f;
    auto c = f.backends->add("alpha", "fake", [] { return std::make_unique<FakeBackend>("fake"); });
    auto tool = make_search_tool(make_search_backend_provider(f.backends));

    const auto described = [&tool] {
        return tool->parameters_schema()["properties"]["mode"]["description"].get<std::string>();
    };
    ASSERT(described().find("fake") != std::string::npos);

    c.remove();
    ASSERT(described().find("fake") == std::string::npos);
    ASSERT(described().find("No search backend is currently enabled") != std::string::npos);
}

TEST(runtime_search_mode_follows_plugin_state) {
    ScratchConfig scratch("runtime_state");
    ToolRegistry tools;
    Config cfg;
    Workspace ws;
    PluginRuntime runtime(tools, cfg, ws);
    runtime.add(std::make_shared<BackendPlugin>());
    runtime.start(false);

    ASSERT_TRUE(runtime.status("fake_backend").enabled);
    ASSERT_TRUE((bool)runtime.search_backends().create("fake"));

    ASSERT_TRUE(runtime.set_state("fake_backend", false));
    ASSERT_FALSE((bool)runtime.search_backends().create("fake"));
    const std::string reason = runtime.search_backends().unavailable_reason("fake");
    ASSERT(reason.find("fake_backend") != std::string::npos);
    ASSERT(reason.find("/set plugin fake_backend on") != std::string::npos);

    ASSERT_TRUE(runtime.set_state("fake_backend", true));
    ASSERT_TRUE((bool)runtime.search_backends().create("fake"));
}

TEST(bundled_search_backend_plugins_provide_grep_and_semantic) {
    ScratchConfig scratch("bundled");
    ToolRegistry tools;
    Config cfg;
    Workspace ws;
    PluginRuntime runtime(tools, cfg, ws);
    runtime.add_bundled();
    runtime.start(false);

    ASSERT_TRUE(runtime.status("search_grep").enabled);
    ASSERT_TRUE(runtime.status("search_semantic").enabled);
    const auto modes = runtime.search_backends().available();
    ASSERT(std::find(modes.begin(), modes.end(), "grep") != modes.end());
    ASSERT(std::find(modes.begin(), modes.end(), "semantic") != modes.end());

    // Switching one off leaves the other live, and the disabled mode reports
    // its plugin rather than vanishing.
    ASSERT_TRUE(runtime.apply_state("search_semantic", false));
    const auto remaining = runtime.search_backends().available();
    ASSERT(std::find(remaining.begin(), remaining.end(), "grep") != remaining.end());
    ASSERT(std::find(remaining.begin(), remaining.end(), "semantic") == remaining.end());
    ASSERT(runtime.search_backends().unavailable_reason("semantic").find("search_semantic") !=
           std::string::npos);
}
