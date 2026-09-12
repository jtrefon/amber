// PF-1.5 runtime tests: activation installs what a plugin declares, disabling
// unwinds every contribution, and state persists across runs. Hermetic: the
// config root is redirected to a scratch directory, so nothing touches the
// developer's real ~/.config.

#include "agent.h"
#include "agent/dialect.h"
#include "agent/extensions.h"
#include "agent/plugin_runtime.h"
#include "agent/plugins_bundled.h"
#include "agent/tools.h"
#include "fake_llm.h"
#include "plugins/commandcode/commandcode_plugin.h"
#include "plugins/deepseek/deepseek_plugin.h"
#include "plugins/kilocode/kilocode_plugin.h"
#include "plugins/metrics/metrics_plugin.h"
#include "plugins/opencode_go/opencode_go_plugin.h"
#include "plugins/openrouter/openrouter_plugin.h"
#include "test_util.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <atomic>
#include <chrono>
#include <optional>
#include <thread>
#include <unistd.h>

using namespace agent;

namespace {

namespace fs = std::filesystem;

// Redirect XDG_CONFIG_HOME for the lifetime of the test so per-plugin state
// files land in a scratch tree.
class ScratchConfig {
public:
    explicit ScratchConfig(const std::string& name)
        : dir_(fs::temp_directory_path() / ("amber_plugin_test_" + name)) {
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
        caps.push_back(
            std::make_unique<PromptBlockCapability>("block", priority_, [this] { return text_; }));
        caps.push_back(std::make_unique<StatusSegmentCapability>(
            "block_seg", priority_, /*drop_priority=*/0,
            [this](const StatusSnapshot&) { return StatusText{text_, StatusTone::Dim}; }));
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

// Declares a wallet whose fetch reports whether it saw the live config.
class WalletProbePlugin : public IPlugin {
public:
    explicit WalletProbePlugin(std::string id = "walletprobe", double amount = 42.5)
        : id_(std::move(id)), amount_(amount) {}

    std::string id() const override { return id_; }
    std::string version() const override { return "1.0.0"; }
    std::string name() const override { return "Wallet probe"; }

    bool initialize(const PluginContext&) override { return true; }
    void shutdown() override {}

    std::vector<std::unique_ptr<Capability>> capabilities() override {
        std::vector<std::unique_ptr<Capability>> caps;
        caps.push_back(std::make_unique<WalletCapability>(
            [amount = amount_](const Config& cfg) -> std::optional<double> {
                ++fetches;
                last_provider = cfg.provider_name;
                last_key = cfg.api_key;
                if (cfg.api_key.empty())
                    return std::nullopt;
                return amount;
            }));
        return caps;
    }

    static inline std::string last_provider;
    static inline std::string last_key;
    static inline int fetches = 0;

private:
    std::string id_;
    double amount_;
};

// A wallet whose fetch takes long enough to still be running when the runtime
// is destroyed: the lifetime guard for the detached worker.
class SlowWalletPlugin : public IPlugin {
public:
    explicit SlowWalletPlugin(std::shared_ptr<std::atomic<bool>> done) : done_(std::move(done)) {}

    std::string id() const override { return "slowwallet"; }
    std::string version() const override { return "1.0.0"; }
    std::string name() const override { return "Slow wallet probe"; }

    bool initialize(const PluginContext&) override { return true; }
    void shutdown() override {}

    std::vector<std::unique_ptr<Capability>> capabilities() override {
        std::vector<std::unique_ptr<Capability>> caps;
        caps.push_back(std::make_unique<WalletCapability>(
            [done = done_](const Config&) -> std::optional<double> {
                std::this_thread::sleep_for(std::chrono::milliseconds(120));
                done->store(true);
                return 7.0;
            }));
        return caps;
    }

private:
    std::shared_ptr<std::atomic<bool>> done_;
};

// A tool with no behaviour: enough to prove registry ownership across a
// plugin's activate/deactivate cycle.
class StubTool : public Tool {
public:
    explicit StubTool(std::string name) : name_(std::move(name)) {}
    std::string name() const noexcept override { return name_; }
    std::string description() const noexcept override { return "stub"; }
    json parameters_schema() const override { return json::object(); }
    ToolResult execute(const json&) const override { return {true, "", "", json{}}; }

private:
    std::string name_;
};

// Contributes one tool under a chosen name, so two plugins can collide on it.
class NamedToolPlugin : public IPlugin {
public:
    NamedToolPlugin(std::string id, std::string tool_name)
        : id_(std::move(id)), tool_name_(std::move(tool_name)) {}

    std::string id() const override { return id_; }
    std::string version() const override { return "1.0.0"; }
    std::string name() const override { return "Named tool probe"; }

    bool initialize(const PluginContext&) override { return true; }
    void shutdown() override {}

    std::vector<std::unique_ptr<Capability>> capabilities() override {
        std::vector<std::unique_ptr<Capability>> caps;
        const std::string tool_name = tool_name_;
        caps.push_back(std::make_unique<ToolCapability>(
            tool_name, [tool_name](PluginServices&) -> std::vector<std::unique_ptr<Tool>> {
                std::vector<std::unique_ptr<Tool>> tools;
                tools.push_back(std::make_unique<StubTool>(tool_name));
                return tools;
            }));
        return caps;
    }

private:
    std::string id_;
    std::string tool_name_;
};

struct Fixture {
    ToolRegistry tools;
    Config cfg;
    Workspace ws;
};

// A plugin whose only capability declines — the shape the core tools plugin has
// whenever a gated tool (todowrite, task) is switched off.
class DecliningCapabilityPlugin : public IPlugin {
public:
    std::string id() const override { return "gated"; }
    std::string version() const override { return "0.1.0"; }
    std::string name() const override { return "Gated"; }
    bool initialize(const PluginContext&) override { return true; }
    void shutdown() override {}
    std::vector<std::unique_ptr<Capability>> capabilities() override {
        std::vector<std::unique_ptr<Capability>> caps;
        caps.push_back(std::make_unique<ToolCapability>(
            "gated", [](PluginServices&) -> std::vector<std::unique_ptr<Tool>> { return {}; }));
        return caps;
    }
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

TEST(runtime_declined_capability_does_not_deactivate_the_plugin) {
    // Regression: a plugin that ships a gated capability (a tool that is absent
    // until a config flag turns it on) must stay active when the capability
    // declines. Treating "nothing to install" as a failure deactivated the
    // whole plugin — which, for the core tool set, left the agent with no tools
    // at all on a default configuration.
    ScratchConfig scratch("decline");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add(std::make_shared<DecliningCapabilityPlugin>());
    runtime.start();

    ASSERT_TRUE(runtime.status("gated").enabled);
    ASSERT_EQ(f.tools.snapshot_tools().size(), 0u);
    ASSERT_TRUE(runtime.contributions().empty());
}

TEST(explicit_dialect_overrides_a_disabled_flavor) {
    // Regression: the disabled-flavor guard tested the constructor parameter
    // *after* it had been moved into the member. A moved-from unique_ptr reads
    // as null, so the guard was unconditionally true and the check ran even
    // when the caller supplied its own dialect — the explicit protocol was
    // thrown away in favour of a refusal.
    ScratchConfig scratch("explicit-dialect");
    register_dialect("probe-flavor", [] { return make_dialect("openai"); }, "probe-plugin");
    unregister_dialects_for("probe-plugin");

    Config cfg;
    cfg.flavor = "probe-flavor";
    ASSERT_FALSE(flavor_unavailable_reason(cfg.flavor).empty());

    // Nothing supplied: a disabled flavor must refuse loudly.
    bool refused = false;
    try {
        HttpLLMClient client(cfg);
    } catch (const std::exception&) {
        refused = true;
    }
    ASSERT_TRUE(refused);

    // An explicit dialect wins: the caller already resolved the protocol.
    bool refused_with_explicit = false;
    try {
        HttpLLMClient client(cfg, make_dialect("openai"));
    } catch (const std::exception&) {
        refused_with_explicit = true;
    }
    ASSERT_FALSE(refused_with_explicit);
}

TEST(runtime_tool_plugins_are_active_on_a_default_config) {
    // Regression: the core tool set is a plugin now, so a default config (plan
    // and task tools off) must still leave the harness with its read/write/
    // search/bash/process tools. A declining gated capability previously failed
    // the whole plugin, and the agent came up with no tools at all.
    ScratchConfig scratch("coretools");
    Fixture f;
    JobService jobs;
    TodoStore todos;
    SubAgentExecutor subagents;
    HostServices host{&jobs, &todos, &subagents, &f.cfg.cancel_token};

    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.attach_host_services(host);
    runtime.add_bundled();
    runtime.start();

    ASSERT_TRUE(runtime.status("tool_read").enabled);
    ASSERT_TRUE(runtime.status("tool_search").enabled);
    ASSERT_TRUE((bool)f.tools.find("read"));
    ASSERT_TRUE((bool)f.tools.find("write"));
    ASSERT_TRUE((bool)f.tools.find("search"));
    ASSERT_TRUE((bool)f.tools.find("bash"));
    ASSERT_TRUE((bool)f.tools.find("process_start"));
    // The plan and task tools are ordinary plugin contributions now: present
    // by default, absent when their plugin is switched off.
    ASSERT_TRUE((bool)f.tools.find("todowrite"));
    ASSERT_TRUE((bool)f.tools.find("task"));
}

TEST(runtime_tool_plugin_state_controls_the_plan_and_task_tools) {
    ScratchConfig scratch("coretools-gated");
    Fixture f;
    JobService jobs;
    TodoStore todos;
    SubAgentExecutor subagents;
    HostServices host{&jobs, &todos, &subagents, &f.cfg.cancel_token};

    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.attach_host_services(host);
    runtime.add_bundled();
    runtime.start();

    ASSERT_TRUE((bool)f.tools.find("todowrite"));
    ASSERT_TRUE((bool)f.tools.find("task"));

    // Disabling the plugin takes its tool back out, and only its tool.
    ASSERT_TRUE(runtime.set_state("tool_plan", false));
    ASSERT_FALSE((bool)f.tools.find("todowrite"));
    ASSERT_TRUE((bool)f.tools.find("task"));
}

// The audit answers the question a toggle raises: what can the harness no
// longer do? Computed from the live registry, so it can never describe a
// configuration that has moved on.
TEST(runtime_audit_follows_the_enabled_toolset) {
    ScratchConfig scratch("audit");
    Fixture f;
    JobService jobs;
    TodoStore todos;
    SubAgentExecutor subagents;
    HostServices host{&jobs, &todos, &subagents, &f.cfg.cancel_token};

    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.attach_host_services(host);
    runtime.add_bundled();
    runtime.start();

    // The shipped set is complete: nothing to warn about.
    ASSERT_TRUE(runtime.audit().empty());

    // Take away the only tool that can read, and the audit says so.
    ASSERT_TRUE(runtime.set_state("tool_read", false));
    auto findings = runtime.audit();
    ASSERT_EQ(findings.size(), 1u);
    ASSERT_TRUE(findings[0].kind == AuditFinding::Kind::Deficiency);
    ASSERT(findings[0].message.find("read") != std::string::npos);

    // Restoring it clears the warning - on demand means never stale.
    ASSERT_TRUE(runtime.set_state("tool_read", true));
    ASSERT_TRUE(runtime.audit().empty());
}

TEST(runtime_disable_unwinds_every_contribution) {
    ScratchConfig scratch("disable");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    auto plugin = std::make_shared<BlockPlugin>("alpha", "text");
    runtime.add(plugin);
    runtime.start();

    ASSERT_EQ(runtime.prompts().size(), 1u);

    ASSERT_TRUE(runtime.set_state("alpha", false));

    ASSERT_FALSE(runtime.status("alpha").enabled);
    ASSERT_EQ(runtime.prompts().size(), 0u);
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

TEST(runtime_core_ui_installs_through_the_capability_path) {
    ScratchConfig scratch("coreui");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);

    // The owner comes only from PluginServices, so "core" on these entries is
    // evidence they were installed by a StatusSegmentCapability rather than
    // written into the registry by hand. That is the point: the path core UI
    // ships on is the path a plugin's contributions take, so a broken install
    // (the class of bug that once shipped an agent with no tools) cannot hide
    // behind "no plugin uses that capability yet".
    bool saw_core_segment = false;
    for (const auto& item : runtime.status().items()) {
        if (item.owner != std::string("core"))
            continue;
        saw_core_segment = true;
    }
    ASSERT_TRUE(saw_core_segment);

    // The wallet readout and the console are core contributions too.
    const auto segments = runtime.status().render(StatusSnapshot{});
    bool saw_wallet = false;
    for (const auto& segment : segments)
        if (segment.id == std::string("wallet"))
            saw_wallet = true;
    ASSERT_TRUE(saw_wallet);

    const auto panels = runtime.panels().items();
    ASSERT(!panels.empty());
    ASSERT_EQ(panels[0].owner, std::string("core"));
    ASSERT_EQ(panels[0].name, std::string("plugins"));
}

TEST(runtime_contributions_span_every_registry) {
    ScratchConfig scratch("contrib");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add(std::make_shared<BlockPlugin>("alpha", "text"));
    runtime.start();

    bool saw_prompt = false, saw_segment = false;
    for (const auto& item : runtime.contributions()) {
        ASSERT_EQ(item.owner, std::string("alpha"));
        if (item.kind == CapabilityKind::PromptBlock)
            saw_prompt = true;
        if (item.kind == CapabilityKind::StatusSegment)
            saw_segment = true;
    }
    ASSERT_TRUE(saw_prompt);
    ASSERT_TRUE(saw_segment);

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

    agent::PluginManager manager; // discovers nothing on this machine
    runtime.add_external(manager);

    ASSERT_TRUE(runtime.add(std::make_shared<BlockPlugin>("metrics", "impostor"), false) == false);
    auto list = runtime.list();
    ASSERT_EQ(list.size(), 1u);
    ASSERT_EQ(list[0].tier, std::string("bundled"));

    // A distinct external id is accepted and reported as external.
    ASSERT_TRUE(runtime.add(std::make_shared<BlockPlugin>("ext_sample", "x"), false));
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

// A provider plugin registers a wire protocol and its presets. Enabling makes
// the provider real; disabling must take both back and refuse loudly, never
// silently fall back to another protocol (PLG-12, PLG-13).
TEST(runtime_provider_plugin_registers_and_unwinds) {
    ScratchConfig scratch("provider");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start();

    ASSERT_TRUE(runtime.status("gemini").enabled);
    ASSERT_EQ(make_dialect("gemini")->flavor(), std::string("gemini"));
    ASSERT_TRUE(flavor_unavailable_reason("gemini").empty());

    bool listed = false;
    for (const auto& p : plugin_provider_presets())
        if (p.name == "gemini") {
            listed = true;
            ASSERT_EQ(p.flavor, std::string("gemini"));
            ASSERT_EQ(p.default_model, std::string("gemini-2.5-pro"));
        }
    ASSERT(listed);

    // Disabling: the dialect and presets go, and the flavor refuses.
    ASSERT_TRUE(runtime.set_state("gemini", false));
    for (const auto& p : plugin_provider_presets())
        ASSERT(p.name != std::string("gemini"));
    const std::string reason = flavor_unavailable_reason("gemini");
    ASSERT_FALSE(reason.empty());
    ASSERT(reason.find("gemini") != std::string::npos);
    ASSERT(reason.find("/set plugin gemini on") != std::string::npos);

    // The client refuses to silently speak another protocol...
    bool threw = false;
    try {
        Config cfg = f.cfg;
        cfg.flavor = "gemini";
        HttpLLMClient client(cfg);
    } catch (const std::exception& e) {
        threw = true;
        ASSERT(std::string(e.what()).find("disabled") != std::string::npos);
    }
    ASSERT(threw);

    // ...while a flavor nobody ever provided still falls back (a typo in a
    // provider file must not break the session).
    Config typo = f.cfg;
    typo.flavor = "no-such-flavor";
    HttpLLMClient fallback(typo);
    ASSERT_TRUE(flavor_unavailable_reason("no-such-flavor").empty());

    // Re-enabling restores both halves.
    ASSERT_TRUE(runtime.set_state("gemini", true));
    ASSERT_TRUE(flavor_unavailable_reason("gemini").empty());
    ASSERT_EQ(make_dialect("gemini")->flavor(), std::string("gemini"));
}

// A plugin must read the host's configuration through the context it was
// given, at the moment it needs it. Caching the Config pointer at activation
// is a trap: hosts construct and start the runtime before they can hand over
// the config they actually mutate (the TUI takes its Config by value), so the
// cached pointer keeps pointing at the runtime's startup copy.
//
// This is the kilo wallet regression: the balance readout disappeared because
// the plugin resolved its token from a stale copy.
TEST(runtime_wallet_reads_the_config_attached_after_start) {
    ScratchConfig scratch("live_config");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add(std::make_shared<WalletProbePlugin>());
    runtime.start(); // activate before the host attaches (the old bug order)

    // The host's real configuration arrives afterwards. The wallet must be
    // fetched with THIS config, not the runtime's startup copy.
    Config live;
    live.provider_name = "walletprobe";
    live.api_key = "kilo-jwt";
    runtime.attach_config(live);
    runtime.perform_wallet_refresh();

    ASSERT_EQ(WalletProbePlugin::last_provider, std::string("walletprobe"));
    ASSERT_EQ(WalletProbePlugin::last_key, std::string("kilo-jwt"));
    const auto view = runtime.wallet();
    ASSERT_TRUE(view.supported);
    ASSERT_TRUE(view.ready);
    ASSERT_FALSE(view.failed);
    ASSERT_EQ(view.amount, 42.5);
}

TEST(wallet_registry_installs_and_unwinds) {
    ToolRegistry tools;
    PromptRegistry prompts;
    StatusRegistry status;
    PanelRegistry panels;
    WalletRegistry wallets;
    AllowanceRegistry allowances;
    EventBus bus;
    PluginServices services(tools, prompts, status, panels, wallets, allowances, bus);
    services.set_owner("acme");

    WalletCapability cap([](const Config&) -> std::optional<double> { return 7.0; });
    InstallResult r = cap.install(services);
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.contribution.kind == CapabilityKind::Wallet);
    ASSERT(wallets.find("acme") != nullptr);

    r.contribution.remove();
    ASSERT_TRUE(wallets.find("acme") == nullptr);
    ASSERT_EQ(wallets.size(), 0u);
}

// The bar has no room to spare: the wallet is the first segment to give up its
// columns, and the display switch hides it entirely.
TEST(runtime_wallet_segment_renders_amount_dash_and_hides) {
    ScratchConfig scratch("wallet_segment");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add(std::make_shared<WalletProbePlugin>());
    runtime.start();

    Config live;
    live.provider_name = "other"; // a provider with no wallet declared
    live.api_key = "probe-key";
    runtime.attach_config(live);

    // Returned by value: the segments live in a temporary vector.
    const auto find_wallet = [&runtime]() -> std::optional<StatusSegment> {
        for (const auto& s : runtime.status().render(StatusSnapshot{}))
            if (s.id == "wallet")
                return s;
        return std::nullopt;
    };

    // Enabled, provider declares nothing: the honest dash.
    std::optional<StatusSegment> seg = find_wallet();
    ASSERT(seg.has_value());
    ASSERT(seg->text.find('-') != std::string::npos);
    ASSERT_EQ(seg->drop_priority, 9); // drops before everything else

    // The switch hides it completely.
    live.wallet_enabled = false;
    ASSERT_FALSE(find_wallet().has_value());
    live.wallet_enabled = true;

    // A provider that declares a wallet, once fetched: just the symbol and
    // the amount, no words.
    live.provider_name = "walletprobe";
    runtime.perform_wallet_refresh();
    seg = find_wallet();
    ASSERT(seg.has_value());
    ASSERT_EQ(seg->text, std::string("  $42.50"));
}

// A provider that declares no wallet yields no stale value from the previous
// one: switching to it clears the readout.
TEST(runtime_wallet_clears_when_switching_to_a_provider_without_one) {
    ScratchConfig scratch("wallet_switch");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add(std::make_shared<WalletProbePlugin>());
    runtime.start();

    Config live;
    live.provider_name = "walletprobe";
    live.api_key = "probe-key";
    runtime.attach_config(live);
    runtime.perform_wallet_refresh();
    ASSERT_TRUE(runtime.wallet().ready);

    live.provider_name = "gemini"; // declares no wallet
    runtime.perform_wallet_refresh();
    const auto view = runtime.wallet();
    ASSERT_FALSE(view.supported);
    ASSERT_FALSE(view.ready);
}

// The wallet refreshes when a turn ends, not on a timer.
TEST(runtime_turn_end_requests_a_wallet_refresh) {
    ScratchConfig scratch("wallet_turn");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add(std::make_shared<WalletProbePlugin>());
    runtime.start();
    Config live;
    live.provider_name = "walletprobe";
    live.api_key = "probe-key";
    runtime.attach_config(live);
    runtime.perform_wallet_refresh();

    WalletProbePlugin::fetches = 0;
    TurnEndedEvent ended;
    Event raw{EventType::AgentTurnEnd, &ended, false};
    runtime.events().fire(EventType::AgentTurnEnd, raw);
    // The turn boundary only marks it stale; the tick (which the host drives)
    // is what performs the fetch, so no I/O happens on the agent thread.
    ASSERT_EQ(WalletProbePlugin::fetches, 0);
}

// The balance endpoint is a fixed kilo.ai API, not the gateway the provider
// chats through. Deriving it from api_base produced
// ".../api/gateway/profile/balance", which 404s — so the readout never had a
// value to show, no matter how valid the key was.
TEST(kilocode_balance_url_is_the_api_not_the_gateway) {
    const std::string url = plugins::kilocode_balance_url();
    ASSERT_EQ(url, std::string("https://api.kilo.ai/api/profile/balance"));
    ASSERT(url.find("/gateway") == std::string::npos);
}

// OpenRouter reports a per-key spend cap rather than an account balance. The
// shape is pinned here because it is what a stored inference key can see.
TEST(openrouter_wallet_parses_the_key_allowance) {
    ASSERT_EQ(plugins::openrouter_key_url("https://openrouter.ai/api/v1"),
              std::string("https://openrouter.ai/api/v1/key"));
    // The default base is used when the provider has none configured.
    ASSERT_EQ(plugins::openrouter_key_url(""), std::string("https://openrouter.ai/api/v1/key"));

    // The direct field.
    const auto direct = plugins::parse_openrouter_key(
        R"({"data":{"limit":20,"usage":7.5,"limit_remaining":12.5}})");
    ASSERT(direct.has_value());
    ASSERT_EQ(*direct, 12.5);

    // Only the cap and the spend: the remainder is arithmetic.
    const auto derived = plugins::parse_openrouter_key(R"({"data":{"limit":20,"usage":7.5}})");
    ASSERT(derived.has_value());
    ASSERT_EQ(*derived, 12.5);

    // An uncapped key has no "remaining": nothing honest to show.
    ASSERT_FALSE(plugins::parse_openrouter_key(
                     R"({"data":{"limit":null,"usage":7.5,"limit_remaining":null}})")
                     .has_value());

    // Garbage is not a number.
    ASSERT_FALSE(plugins::parse_openrouter_key("not json").has_value());
    ASSERT_FALSE(plugins::parse_openrouter_key(R"({"data":[]})").has_value());
}

// Both wallets are contributions like any other: they appear in the registry
// and disappear with their plugin.
TEST(runtime_exposes_the_builtin_wallets) {
    ScratchConfig scratch("wallets");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start();

    ASSERT_TRUE(runtime.wallets().find("kilocode") != nullptr);
    ASSERT_TRUE(runtime.wallets().find("openrouter") != nullptr);
    // Providers with no account API to ask declare none.
    ASSERT_TRUE(runtime.wallets().find("gemini") == nullptr);
    ASSERT_TRUE(runtime.wallets().find("anthropic") == nullptr);
    ASSERT_TRUE(runtime.wallets().find("custom") == nullptr);

    // The all-contributions view is ledger-driven, so it must see wallets too
    // — that is the view the "nothing survives disable" invariant is asserted
    // against, and it silently missed three capability kinds before.
    bool saw_wallet = false;
    for (const auto& item : runtime.contributions())
        if (item.kind == CapabilityKind::Wallet && item.owner == "openrouter")
            saw_wallet = true;
    ASSERT(saw_wallet);

    // Disabling the provider plugin takes its wallet with it.
    ASSERT_TRUE(runtime.set_state("openrouter", false));
    ASSERT_TRUE(runtime.wallets().find("openrouter") == nullptr);
    ASSERT_TRUE(runtime.wallets().find("kilocode") != nullptr);
    for (const auto& item : runtime.contributions())
        ASSERT(item.owner != std::string("openrouter"));
}

// A plugin that *observes* rather than contributes subscribes to the bus
// directly, which the capability ledger knows nothing about. Disabling it must
// still leave nothing behind: the framework's promise is that a disabled plugin
// costs nothing, and a callback outliving its plugin is the same leak the
// ledger prevents on the capability side.
TEST(runtime_disable_releases_an_observers_subscriptions) {
    ScratchConfig scratch("observer_off");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start();

    // The metrics plugin observes the loop; while it is on its subscriptions
    // are live. (AgentTurnEnd also carries the runtime's own wallet refresh,
    // so it is checked by count rather than presence.)
    ASSERT_TRUE(runtime.events().has_subscribers(EventType::AgentTurnStart));
    ASSERT_TRUE(runtime.events().has_subscribers(EventType::ToolCallBefore));
    ASSERT_TRUE(runtime.events().has_subscribers(EventType::ToolCallAfter));

    ASSERT_TRUE(runtime.set_state("metrics", false));

    ASSERT_FALSE(runtime.events().has_subscribers(EventType::AgentTurnStart));
    ASSERT_FALSE(runtime.events().has_subscribers(EventType::ToolCallBefore));
    ASSERT_FALSE(runtime.events().has_subscribers(EventType::ToolCallAfter));

    // And its own state is reset, so a re-enable starts from zero.
    auto* metrics = dynamic_cast<plugins::MetricsPlugin*>(runtime.find("metrics"));
    ASSERT(metrics != nullptr);
    ASSERT_EQ(metrics->stats().turns, 0);
}

// The wallet fetch runs on a detached worker, so it must not depend on the
// runtime outliving it. The worker captures the shared state, a copy of the
// fetch and a copy of the config — never the runtime — so a fetch still running
// when the runtime is destroyed completes safely instead of dereferencing freed
// memory. (Under ASAN this is the test that would catch a regression.)
TEST(runtime_wallet_fetch_survives_runtime_destruction) {
    ScratchConfig scratch("wallet_lifetime");
    auto done = std::make_shared<std::atomic<bool>>(false);
    {
        Fixture f;
        PluginRuntime runtime(f.tools, f.cfg, f.ws);
        runtime.add(std::make_shared<SlowWalletPlugin>(done));
        runtime.start();

        Config live;
        live.provider_name = "slowwallet";
        live.api_key = "probe-key";
        runtime.attach_config(live); // requests a refresh
        runtime.tick();              // schedules it on the worker
    }
    // The runtime is gone; the fetch is still in flight and must finish safely.
    for (int i = 0; i < 200 && !done->load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    ASSERT(done->load());
}

// A fetch that lands after the provider changed must not be shown under the new
// provider: the bar would report another account's balance as this one's.
TEST(runtime_wallet_result_from_a_previous_provider_is_not_shown) {
    ScratchConfig scratch("wallet_stale");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add(std::make_shared<WalletProbePlugin>("probe_a", 10.0));
    runtime.add(std::make_shared<WalletProbePlugin>("probe_b", 20.0));
    runtime.start();

    Config a;
    a.provider_name = "probe_a";
    a.api_key = "key";
    runtime.attach_config(a);
    runtime.perform_wallet_refresh();
    ASSERT_TRUE(runtime.wallet().ready);
    ASSERT_EQ(runtime.wallet().amount, 10.0);

    // Switch: probe_b declares a wallet too, but nothing has been fetched for
    // it yet, so the readout must be "not fetched" rather than probe_a's 10.0.
    Config b;
    b.provider_name = "probe_b";
    b.api_key = "key";
    runtime.attach_config(b);
    const auto stale = runtime.wallet();
    ASSERT_TRUE(stale.supported);
    ASSERT_FALSE(stale.ready);
    ASSERT_EQ(stale.holder, std::string("probe_b"));

    // Once fetched, the new provider's own value appears.
    runtime.perform_wallet_refresh();
    const auto fresh = runtime.wallet();
    ASSERT_TRUE(fresh.ready);
    ASSERT_EQ(fresh.amount, 20.0);
}

TEST(runtime_find_returns_null_for_unknown_plugins) {
    ScratchConfig scratch("find_unknown");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    ASSERT_TRUE(runtime.find("metrics") != nullptr);
    ASSERT_TRUE(runtime.find("nope") == nullptr);
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
    if (getcwd(cwd, sizeof cwd))
        Workspace::set_root(cwd);

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

// Disabling one plugin must leave another plugin's identically-named tool
// alone. This is the ledger's headline promise seen through the runtime:
// unwinding a plugin restores the registries to their pre-activation state and
// never touches a neighbour's contribution.
TEST(runtime_disable_cannot_remove_another_plugins_tool) {
    ScratchConfig scratch("tool_owner");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add(std::make_shared<NamedToolPlugin>("alpha", "probe"));
    runtime.add(std::make_shared<NamedToolPlugin>("beta", "probe"));
    runtime.start();

    ASSERT_TRUE((bool)f.tools.find("probe"));

    // beta registered last, so the live "probe" is beta's. Disabling alpha must
    // not take it away — alpha's own instance was superseded on registration.
    ASSERT_TRUE(runtime.set_state("alpha", false));
    ASSERT_TRUE((bool)f.tools.find("probe"));

    // Disabling beta removes its own.
    ASSERT_TRUE(runtime.set_state("beta", false));
    ASSERT_FALSE((bool)f.tools.find("probe"));
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

// ---------------------------------------------------------------------------
// Allowance capability tests
// ---------------------------------------------------------------------------

class AllowanceProbePlugin : public IPlugin {
public:
    static AllowanceSnapshot fixed_snapshot;

    std::string id() const override { return "allowanceprobe"; }
    std::string version() const override { return "1.0.0"; }
    std::string name() const override { return "Allowance probe"; }

    bool initialize(const PluginContext&) override { return true; }
    void shutdown() override {}

    std::vector<std::unique_ptr<Capability>> capabilities() override {
        std::vector<std::unique_ptr<Capability>> caps;
        caps.push_back(std::make_unique<AllowanceCapability>(
            [](const Config&) -> std::optional<AllowanceSnapshot> { return fixed_snapshot; }));
        return caps;
    }
};

AllowanceSnapshot AllowanceProbePlugin::fixed_snapshot;

TEST(allowance_registry_installs_and_unwinds) {
    ToolRegistry tools;
    PromptRegistry prompts;
    StatusRegistry status;
    PanelRegistry panels;
    WalletRegistry wallets;
    AllowanceRegistry allowances;
    EventBus bus;
    PluginServices services(tools, prompts, status, panels, wallets, allowances, bus);
    services.set_owner("acme");

    AllowanceCapability cap([](const Config&) -> std::optional<AllowanceSnapshot> {
        AllowanceSnapshot s;
        s.plan = "Pro";
        return s;
    });
    InstallResult r = cap.install(services);
    ASSERT_TRUE(r.ok);
    ASSERT_TRUE(r.contribution.kind == CapabilityKind::Allowance);
    ASSERT(allowances.find("acme") != nullptr);

    r.contribution.remove();
    ASSERT_TRUE(allowances.find("acme") == nullptr);
    ASSERT_EQ(allowances.size(), 0u);
}

TEST(allowance_registry_replaces_existing_for_same_owner) {
    AllowanceRegistry allowances;
    allowances.add("acme", [](const Config&) { return std::nullopt; });
    ASSERT_EQ(allowances.size(), 1u);
    allowances.add("acme", [](const Config&) { return std::nullopt; });
    ASSERT_EQ(allowances.size(), 1u);
}

TEST(allowance_registry_items_reports_kind_and_owner) {
    AllowanceRegistry allowances;
    allowances.add("acme", [](const Config&) { return std::nullopt; });
    auto items = allowances.items();
    ASSERT_EQ(items.size(), 1u);
    ASSERT_TRUE(items[0].kind == CapabilityKind::Allowance);
    ASSERT_EQ(items[0].owner, std::string("acme"));
}

TEST(opencode_go_usage_parser_parses_three_windows) {
    const std::string body = R"({
        "usage": {
            "rolling":  { "status": "ok", "percent": 12, "resetsAt": "2026-09-11T15:00:00Z" },
            "weekly":   { "status": "ok", "percent": 34, "resetsAt": "2026-09-15T00:00:00Z" },
            "monthly":  { "status": "ok", "percent": 56, "resetsAt": "2026-10-01T00:00:00Z" }
        }
    })";
    auto snap = plugins::parse_opencode_go_usage(body);
    ASSERT(snap.has_value());
    ASSERT_EQ(snap->windows.size(), 3u);
    ASSERT_EQ(snap->windows[0].label, std::string("5h"));
    ASSERT_EQ(snap->windows[0].percent_used, 12.0);
    ASSERT_EQ(snap->windows[1].label, std::string("7d"));
    ASSERT_EQ(snap->windows[1].percent_used, 34.0);
    ASSERT_EQ(snap->windows[2].label, std::string("monthly"));
    ASSERT_EQ(snap->windows[2].percent_used, 56.0);
}

TEST(opencode_go_usage_parser_rejects_malformed) {
    ASSERT_FALSE(plugins::parse_opencode_go_usage("").has_value());
    ASSERT_FALSE(plugins::parse_opencode_go_usage("not json").has_value());
    ASSERT_FALSE(plugins::parse_opencode_go_usage("{}").has_value());
    ASSERT_FALSE(plugins::parse_opencode_go_usage(R"({"usage":{}})").has_value());
}

TEST(commandcode_credits_parser_parses_windows_and_credits) {
    const std::string body = R"({
        "credits": {
            "monthlyCredits": 8.50,
            "purchasedCredits": 2.00,
            "freeCredits": 0.50
        },
        "windowLimits": {
            "fiveHour": { "used": 0.50, "cap": 3.00, "resetAt": 1723468800000 },
            "weekly":   { "used": 1.20, "cap": 6.00, "resetAt": 1723728000000 }
        }
    })";
    auto snap = plugins::parse_commandcode_credits(body);
    ASSERT(snap.has_value());
    ASSERT_EQ(snap->windows.size(), 3u);
    ASSERT(snap->credits_balance.has_value());
    ASSERT_EQ(*snap->credits_balance, 11.0);
    ASSERT_EQ(snap->windows[1].label, std::string("5h"));
    ASSERT(snap->windows[1].percent_used > 16.0);
    ASSERT(snap->windows[1].percent_used < 17.0);
    ASSERT_EQ(snap->windows[1].remaining, 2.5);
    ASSERT_EQ(snap->windows[2].label, std::string("7d"));
    ASSERT_EQ(snap->windows[2].percent_used, 20.0);
}

TEST(commandcode_credits_parser_handles_nested_window_limits) {
    const std::string body = R"({
        "credits": {
            "monthlyCredits": 5.0,
            "windowLimits": {
                "fiveHour": { "used": 1.0, "cap": 4.0, "resetAt": 0 }
            }
        }
    })";
    auto snap = plugins::parse_commandcode_credits(body);
    ASSERT(snap.has_value());
    ASSERT_EQ(snap->windows.size(), 2u);
    ASSERT_EQ(snap->windows[1].label, std::string("5h"));
    ASSERT_EQ(snap->windows[1].percent_used, 25.0);
}

TEST(commandcode_credits_parser_rejects_malformed) {
    ASSERT_FALSE(plugins::parse_commandcode_credits("").has_value());
    ASSERT_FALSE(plugins::parse_commandcode_credits("not json").has_value());
    ASSERT_FALSE(plugins::parse_commandcode_credits("{}").has_value());
}

TEST(deepseek_balance_parser_parses_total) {
    const std::string body = R"({
        "is_available": true,
        "balance_infos": [
            { "currency": "CNY", "total_balance": "10.50" }
        ]
    })";
    ASSERT_EQ(plugins::parse_deepseek_balance(body), 10.5);
}

TEST(deepseek_balance_parser_sums_multiple_currencies) {
    const std::string body = R"({
        "is_available": true,
        "balance_infos": [
            { "currency": "CNY", "total_balance": "10.00" },
            { "currency": "USD", "total_balance": "5.00" }
        ]
    })";
    ASSERT_EQ(plugins::parse_deepseek_balance(body), 15.0);
}

TEST(deepseek_balance_parser_rejects_malformed) {
    ASSERT_EQ(plugins::parse_deepseek_balance(""), -1.0);
    ASSERT_EQ(plugins::parse_deepseek_balance("not json"), -1.0);
    ASSERT_EQ(plugins::parse_deepseek_balance(R"({"balance_infos":[]})"), -1.0);
}

TEST(allowance_segment_shows_closest_window) {
    ScratchConfig scratch("allowance_closest");
    Fixture f;
    AllowanceProbePlugin::fixed_snapshot = AllowanceSnapshot{};
    AllowanceProbePlugin::fixed_snapshot.plan = "Go";
    AllowanceProbePlugin::fixed_snapshot.unit = "percent";
    AllowanceWindow w5h;
    w5h.label = "5h";
    w5h.percent_used = 80.0;
    AllowanceWindow w7d;
    w7d.label = "7d";
    w7d.percent_used = 30.0;
    AllowanceWindow wM;
    wM.label = "monthly";
    wM.percent_used = 20.0;
    AllowanceProbePlugin::fixed_snapshot.windows = {w5h, w7d, wM};

    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add(std::make_shared<AllowanceProbePlugin>());
    runtime.start();
    Config live;
    live.provider_name = "allowanceprobe";
    live.api_key = "key";
    runtime.attach_config(live);
    runtime.perform_allowance_refresh();

    const auto find_allowance = [&runtime]() -> std::optional<StatusSegment> {
        for (const auto& s : runtime.status().render(StatusSnapshot{}))
            if (s.id == "allowance")
                return s;
        return std::nullopt;
    };
    auto seg = find_allowance();
    ASSERT(seg.has_value());
    ASSERT(seg->text.find("80%") != std::string::npos);
    ASSERT(seg->text.find("5h") != std::string::npos);
    ASSERT(seg->tone == StatusTone::Warn);
}

TEST(allowance_segment_breaks_ties_by_shortest_label) {
    ScratchConfig scratch("allowance_tie");
    Fixture f;
    AllowanceProbePlugin::fixed_snapshot = AllowanceSnapshot{};
    AllowanceWindow w5h;
    w5h.label = "5h";
    w5h.percent_used = 50.0;
    AllowanceWindow w7d;
    w7d.label = "7d";
    w7d.percent_used = 50.0;
    AllowanceProbePlugin::fixed_snapshot.windows = {w7d, w5h};

    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add(std::make_shared<AllowanceProbePlugin>());
    runtime.start();
    Config live;
    live.provider_name = "allowanceprobe";
    live.api_key = "key";
    runtime.attach_config(live);
    runtime.perform_allowance_refresh();

    const auto find_allowance = [&runtime]() -> std::optional<StatusSegment> {
        for (const auto& s : runtime.status().render(StatusSnapshot{}))
            if (s.id == "allowance")
                return s;
        return std::nullopt;
    };
    auto seg = find_allowance();
    ASSERT(seg.has_value());
    ASSERT(seg->text.find("5h") != std::string::npos);
}

TEST(allowance_segment_hides_when_disabled) {
    ScratchConfig scratch("allowance_hide");
    Fixture f;
    AllowanceProbePlugin::fixed_snapshot = AllowanceSnapshot{};
    AllowanceWindow w;
    w.label = "5h";
    w.percent_used = 50.0;
    AllowanceProbePlugin::fixed_snapshot.windows = {w};

    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add(std::make_shared<AllowanceProbePlugin>());
    runtime.start();
    Config live;
    live.provider_name = "allowanceprobe";
    live.api_key = "key";
    live.allowance_enabled = false;
    runtime.attach_config(live);
    runtime.perform_allowance_refresh();

    const auto find_allowance = [&runtime]() -> std::optional<StatusSegment> {
        for (const auto& s : runtime.status().render(StatusSnapshot{}))
            if (s.id == "allowance")
                return s;
        return std::nullopt;
    };
    ASSERT_FALSE(find_allowance().has_value());
}

TEST(allowance_segment_shows_dash_for_unsupported_provider) {
    ScratchConfig scratch("allowance_dash");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.start();
    Config live;
    live.provider_name = "custom";
    runtime.attach_config(live);

    const auto find_allowance = [&runtime]() -> std::optional<StatusSegment> {
        for (const auto& s : runtime.status().render(StatusSnapshot{}))
            if (s.id == "allowance")
                return s;
        return std::nullopt;
    };
    auto seg = find_allowance();
    ASSERT(seg.has_value());
    ASSERT(seg->text.find('-') != std::string::npos);
}

TEST(bundled_plugins_include_new_providers) {
    ScratchConfig scratch("new_providers");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    auto list = runtime.list();
    std::vector<std::string> ids;
    ids.reserve(list.size());
    for (const auto& p : list)
        ids.push_back(p.id);
    auto has = [&](const std::string& id) {
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    };
    ASSERT(has("opencode_go"));
    ASSERT(has("opencode_zen"));
    ASSERT(has("commandcode"));
    ASSERT(has("deepseek"));
}

// The benchmark harness switches plugins off to measure the difference. That
// experiment must not become the user's saved preference, and it must not read
// one either: a result that depended on this machine's plugin state would not
// be reproducible on any other.
TEST(apply_state_changes_the_toolset_without_writing_state) {
    ScratchConfig scratch("apply_state");
    Fixture f;
    JobService jobs;
    TodoStore todos;
    SubAgentExecutor subagents;
    HostServices host{&jobs, &todos, &subagents, &f.cfg.cancel_token};
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.attach_host_services(host);
    runtime.add_bundled();
    runtime.start(/*use_persisted_state=*/false);

    ASSERT_TRUE((bool)f.tools.find("search"));
    ASSERT_TRUE(runtime.apply_state("tool_search", false));
    ASSERT_FALSE((bool)f.tools.find("search"));

    // Nothing was persisted: the file is what a user's toggle would write, and
    // this was not a toggle.
    ASSERT_FALSE(fs::exists(agent::global_config_dir() + "/plugins/tool_search/plugin.conf"));

    ASSERT_TRUE(runtime.apply_state("tool_search", true));
    ASSERT_TRUE((bool)f.tools.find("search"));
}

TEST(start_ignores_persisted_state_when_told_to) {
    ScratchConfig scratch("start_defaults");
    Fixture f;
    JobService jobs;
    TodoStore todos;
    SubAgentExecutor subagents;
    HostServices host{&jobs, &todos, &subagents, &f.cfg.cancel_token};

    // A saved preference that says "off", as a previous session would leave.
    {
        PluginRuntime writer(f.tools, f.cfg, f.ws);
        writer.add_bundled();
        ASSERT_TRUE(writer.set_state("tool_search", false));
        ASSERT_FALSE((bool)f.tools.find("search"));
    }

    // The harness starts from the shipped configuration regardless.
    Fixture fresh;
    PluginRuntime bench(fresh.tools, f.cfg, fresh.ws);
    bench.attach_host_services(host);
    bench.add_bundled();
    bench.start(/*use_persisted_state=*/false);
    ASSERT_TRUE((bool)fresh.tools.find("search"));

    // A host still honours what the user chose.
    Fixture hosted;
    PluginRuntime host_rt(hosted.tools, f.cfg, hosted.ws);
    host_rt.attach_host_services(host);
    host_rt.add_bundled();
    host_rt.start();
    ASSERT_FALSE((bool)hosted.tools.find("search"));
}

TEST(disabling_allowance_plugin_unwinds_contribution) {
    ScratchConfig scratch("allowance_unwind");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add(std::make_shared<AllowanceProbePlugin>());
    runtime.start();
    ASSERT(runtime.allowances().find("allowanceprobe") != nullptr);
    ASSERT_TRUE(runtime.set_state("allowanceprobe", false));
    ASSERT(runtime.allowances().find("allowanceprobe") == nullptr);
}
