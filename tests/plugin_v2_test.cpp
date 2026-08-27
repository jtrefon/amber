
#include "agent/plugin_v2.h"
#include "agent/plugin_registry.h"
#include "test_util.h"
#include <memory>
#include <string>
#include <vector>

using namespace agent;

namespace {

class StubPlugin : public IPlugin {
public:
    StubPlugin(std::string id, std::string ver)
        : id_(std::move(id)), version_(std::move(ver)) {}

    std::string id() const override { return id_; }
    std::string version() const override { return version_; }
    std::string name() const override { return "Stub " + id_; }

    bool initialize(const PluginContext&) override {
        initialized_ = true;
        return should_succeed_;
    }

    void shutdown() override { shutdown_ = true; }

    std::vector<Capability> capabilities() const override {
        return capabilities_;
    }

    bool initialized_ = false;
    bool shutdown_ = false;
    bool should_succeed_ = true;
    std::vector<Capability> capabilities_;

private:
    std::string id_;
    std::string version_;
};

class FailingPlugin : public IPlugin {
public:
    std::string id() const override { return "failing"; }
    std::string version() const override { return "1.0.0"; }
    std::string name() const override { return "Failing"; }
    bool initialize(const PluginContext&) override { return false; }
    void shutdown() override {}
    std::vector<Capability> capabilities() const override { return {}; }
};

} // namespace

TEST(plugin_v2_interface_identity) {
    StubPlugin p("myplugin", "2.1.0");
    ASSERT_EQ(p.id(), "myplugin");
    ASSERT_EQ(p.version(), "2.1.0");
    ASSERT_EQ(p.name(), "Stub myplugin");
}

TEST(plugin_v2_initialize_returns_bool) {
    StubPlugin p("ok", "1.0.0");
    EventBus bus;
    ToolRegistry tools;
    Config cfg;
    Workspace ws;
    PluginContext ctx{bus, tools, cfg, ws};
    ASSERT_TRUE(p.initialize(ctx));
    ASSERT_TRUE(p.initialized_);
}

TEST(plugin_v2_initialize_failure) {
    FailingPlugin p;
    EventBus bus;
    ToolRegistry tools;
    Config cfg;
    Workspace ws;
    PluginContext ctx{bus, tools, cfg, ws};
    ASSERT_FALSE(p.initialize(ctx));
}

TEST(plugin_v2_capabilities_empty_by_default) {
    StubPlugin p("empty", "1.0.0");
    ASSERT_TRUE(p.capabilities().empty());
}

TEST(plugin_v2_capability_tool_type) {
    StubPlugin p("withtool", "1.0.0");
    Capability cap;
    cap.type = Capability::Type::Tool;
    cap.name = "my_tool";
    cap.impl = nullptr;
    p.capabilities_.push_back(cap);

    auto caps = p.capabilities();
    ASSERT_EQ(caps.size(), 1u);
    ASSERT_EQ(static_cast<int>(caps[0].type), static_cast<int>(Capability::Type::Tool));
    ASSERT_EQ(caps[0].name, "my_tool");
}

TEST(plugin_v2_capability_multiple_types) {
    StubPlugin p("multi", "1.0.0");
    p.capabilities_.push_back({Capability::Type::Tool, "t1", "desc", nullptr});
    p.capabilities_.push_back({Capability::Type::Hook, "h1", "desc", nullptr});
    p.capabilities_.push_back({Capability::Type::Completion, "c1", "desc", nullptr});

    ASSERT_EQ(p.capabilities().size(), 3u);
}

TEST(plugin_registry_create_empty) {
    PluginRegistry reg;
    ASSERT_TRUE(reg.list().empty());
}

TEST(plugin_registry_register_and_find) {
    PluginRegistry reg;
    auto plugin = std::make_shared<StubPlugin>("test", "1.0.0");
    reg.register_plugin(plugin);

    auto found = reg.find("test");
    ASSERT(found != nullptr);
    ASSERT_EQ(found->id(), "test");
}

TEST(plugin_registry_find_nonexistent_returns_null) {
    PluginRegistry reg;
    ASSERT(reg.find("nope") == nullptr);
}

TEST(plugin_registry_activate_success) {
    PluginRegistry reg;
    auto plugin = std::make_shared<StubPlugin>("act", "1.0.0");
    reg.register_plugin(plugin);
    ASSERT_TRUE(reg.activate("act"));
    ASSERT_EQ(reg.state("act"), PluginRegistry::State::Active);
    ASSERT_TRUE(plugin->initialized_);
}

TEST(plugin_registry_activate_nonexistent_fails) {
    PluginRegistry reg;
    ASSERT_FALSE(reg.activate("nope"));
}

TEST(plugin_registry_activate_failure_marks_failed) {
    PluginRegistry reg;
    auto plugin = std::make_shared<FailingPlugin>();
    reg.register_plugin(plugin);
    ASSERT_FALSE(reg.activate("failing"));
    ASSERT_EQ(reg.state("failing"), PluginRegistry::State::Failed);
}

TEST(plugin_registry_deactivate) {
    PluginRegistry reg;
    auto plugin = std::make_shared<StubPlugin>("deact", "1.0.0");
    reg.register_plugin(plugin);
    reg.activate("deact");
    ASSERT_TRUE(reg.deactivate("deact"));
    ASSERT_EQ(reg.state("deact"), PluginRegistry::State::Deactivated);
    ASSERT_TRUE(plugin->shutdown_);
}

TEST(plugin_registry_deactivate_nonexistent_fails) {
    PluginRegistry reg;
    ASSERT_FALSE(reg.deactivate("nope"));
}

TEST(plugin_registry_list_shows_all) {
    PluginRegistry reg;
    reg.register_plugin(std::make_shared<StubPlugin>("a", "1.0.0"));
    reg.register_plugin(std::make_shared<StubPlugin>("b", "2.0.0"));

    auto all = reg.list();
    ASSERT_EQ(all.size(), 2u);
}

TEST(plugin_registry_shutdown_all) {
    PluginRegistry reg;
    auto p1 = std::make_shared<StubPlugin>("s1", "1.0.0");
    auto p2 = std::make_shared<StubPlugin>("s2", "1.0.0");
    reg.register_plugin(p1);
    reg.register_plugin(p2);
    reg.activate("s1");
    reg.activate("s2");
    reg.shutdown_all();
    ASSERT_TRUE(p1->shutdown_);
    ASSERT_TRUE(p2->shutdown_);
}

TEST(plugin_registry_event_bus_shared) {
    PluginRegistry reg;
    EventBus& bus1 = reg.event_bus();
    EventBus& bus2 = reg.event_bus();
    ASSERT(&bus1 == &bus2);
}


