// PF-1.5 runtime tests: activation installs what a plugin declares, disabling
// unwinds every contribution, and state persists across runs. Hermetic: the
// config root is redirected to a scratch directory, so nothing touches the
// developer's real ~/.config.

#include "agent.h"
#include "agent/dialect.h"
#include "agent/extensions.h"
#include "agent/plugin_console.h"
#include "agent/plugin_runtime.h"
#include "agent/plugins_bundled.h"
#include "agent/ui_services.h"
#include "agent/tools.h"
#include "fake_llm.h"
#include "plugins/commandcode/commandcode_plugin.h"
#include "plugins/coding/coding_library.h"
#include "plugins/deepseek/deepseek_plugin.h"
#include "plugins/git_timeline/git_timeline_plugin.h"
#include "plugins/kilocode/kilocode_plugin.h"
#include "plugins/mcp/mcp_plugin.h"
#include "plugins/metrics/metrics_plugin.h"
#include "plugins/project_instructions/project_instructions_plugin.h"
#include "plugins/project_index/index_store.h"
#include "plugins/project_index/project_index_plugin.h"
#include "plugins/opencode_go/opencode_go_plugin.h"
#include "plugins/openrouter/openrouter_plugin.h"
#include "test_util.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
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

    // The scratch config root: where a test writes the files a user would
    // (provider confs, mcp server confs, plugin state).
    const std::string root() const { return dir_.string(); }

private:
    fs::path dir_;
};

// Point a plugin at the stdio MCP fixture server through the same config file
// a user writes: <XDG_CONFIG_HOME>/amber/mcp/<name>.conf.
void write_mcp_server_conf(const std::string& config_root, const std::string& name,
                           const std::string& command, const std::string& cwd) {
    const fs::path dir = fs::path(config_root) / "amber" / "mcp";
    fs::create_directories(dir);
    std::ofstream f(dir / (name + ".conf"));
    f << "type=stdio\n"
      << "command=" << command << "\n"
      << "cwd=" << cwd << "\n"
      << "enabled=1\nauto_connect=1\ntrusted=1\n";
}

// True when a registry entry is owned by `owner` (the plugin id a contribution
// was installed under).
bool segment_owned_by(const StatusRegistry& status, const std::string& owner) {
    for (const auto& item : status.items())
        if (item.owner == owner)
            return true;
    return false;
}

bool command_root_present(const CommandRegistry& commands, const std::string& root) {
    for (const auto& entry : commands.all())
        if (entry.spec.name == root)
            return true;
    return false;
}

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
            [amount = amount_](const Config& cfg) -> std::optional<WalletSnapshot> {
                ++fetches;
                last_provider = cfg.provider_name;
                last_key = cfg.api_key;
                if (cfg.api_key.empty())
                    return std::nullopt;
                return WalletSnapshot::of_balance(amount);
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
            [done = done_](const Config&) -> std::optional<WalletSnapshot> {
                std::this_thread::sleep_for(std::chrono::milliseconds(120));
                done->store(true);
                return WalletSnapshot::of_balance(7.0);
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

// A plugin whose tool set is DISCOVERED: it adds one tool at start and keeps
// the port, the way a source adds again when another subject appears (a server
// that connects, a session that opens).
class ToolSourcePlugin : public IPlugin {
public:
    std::string id() const override { return "source"; }
    std::string version() const override { return "0.1.0"; }
    std::string name() const override { return "Tool source probe"; }

    bool initialize(const PluginContext&) override { return true; }
    void shutdown() override {}

    std::vector<std::unique_ptr<Capability>> capabilities() override {
        std::vector<std::unique_ptr<Capability>> caps;
        caps.push_back(std::make_unique<ToolSourceCapability>(
            "source",
            [this](PluginServices&, ToolPort& port) {
                port_ = &port;
                ++starts_;
                port.add(std::make_unique<StubTool>("discovered_one"));
            },
            [this] {
                port_ = nullptr;
                ++stops_;
            }));
        return caps;
    }

    ToolPort* port_ = nullptr;
    int starts_ = 0;
    int stops_ = 0;
};

struct Fixture {
    ToolRegistry tools;
    Config cfg;
    Workspace ws;
};

// Every System prompt block the runtime currently holds, concatenated: what a
// tool's prose contributes to the compiled prompt.
std::string system_blocks_text(PluginRuntime& runtime) {
    std::string all;
    for (auto& text : runtime.prompts().render_all(PromptPlacement::System))
        all += text;
    return all;
}

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

// A discovered set is contributed through the runtime's port, which records
// what it carried so deactivation unwinds exactly that — however late a tool
// arrived.
TEST(tool_source_installs_and_unwinds_everything_it_added) {
    ScratchConfig scratch("tool-source");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    auto source = std::make_shared<ToolSourcePlugin>();
    runtime.add(source);
    runtime.start(/*use_persisted_state=*/false);

    ASSERT_EQ(source->starts_, 1);
    ASSERT_TRUE((bool)f.tools.find("discovered_one"));
    ASSERT_TRUE(source->port_ != nullptr);

    // A source keeps adding while it is active: another subject appeared.
    source->port_->add(std::make_unique<StubTool>("discovered_two"));
    ASSERT_TRUE((bool)f.tools.find("discovered_two"));

    ASSERT_TRUE(runtime.set_state("source", false));
    ASSERT_EQ(source->stops_, 1);
    ASSERT_FALSE((bool)f.tools.find("discovered_one"));
    ASSERT_FALSE((bool)f.tools.find("discovered_two"));
}

TEST(tool_port_cannot_remove_another_owners_tool) {
    ScratchConfig scratch("tool-source-owner");
    Fixture f;
    f.tools.register_tool(std::make_unique<StubTool>("shared_name"), "host");
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    auto source = std::make_shared<ToolSourcePlugin>();
    runtime.add(source);
    runtime.start(/*use_persisted_state=*/false);
    ASSERT_TRUE(source->port_ != nullptr);

    // A name this port never added: a no-op, the host's tool stays.
    source->port_->remove("shared_name");
    ASSERT_TRUE((bool)f.tools.find("shared_name"));

    // Its own: removed.
    source->port_->remove("discovered_one");
    ASSERT_FALSE((bool)f.tools.find("discovered_one"));
}

TEST(tool_source_is_re_installable) {
    ScratchConfig scratch("tool-source-replay");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    auto source = std::make_shared<ToolSourcePlugin>();
    runtime.add(source);
    runtime.start(/*use_persisted_state=*/false);
    ASSERT_TRUE((bool)f.tools.find("discovered_one"));

    ASSERT_TRUE(runtime.set_state("source", false));
    ASSERT_FALSE((bool)f.tools.find("discovered_one"));

    // Enabling replays the declaration: start runs again with a fresh port.
    ASSERT_TRUE(runtime.set_state("source", true));
    ASSERT_EQ(source->starts_, 2);
    ASSERT_TRUE((bool)f.tools.find("discovered_one"));
}

// The timeline playbook ships compiled in and travels with its plugin: a fresh
// project has it from the first turn, and disabling the plugin takes it away.
TEST(git_timeline_skill_installs_and_unwinds) {
    ScratchConfig scratch("git-timeline-skill");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add(std::make_shared<plugins::GitTimelinePlugin>());
    const auto has_skill = [] {
        for (const auto& s : agent::system_skills())
            if (s.name == "git-timeline")
                return true;
        return false;
    };
    runtime.start(/*use_persisted_state=*/false);
    ASSERT_TRUE(has_skill());
    ASSERT_TRUE(runtime.set_state("git-timeline", false));
    ASSERT_FALSE(has_skill());
}

// The grammar the resident prose points at: one area per subject, a compact
// key, and the history moves that make a snapshot findable and undoable.
TEST(git_timeline_skill_teaches_the_index_grammar) {
    const std::string body = agent::plugins::render_git_timeline_skill();
    ASSERT(body.find("<area>: <imperative summary>") != std::string::npos);
    ASSERT(body.find("One area per commit") != std::string::npos);
    ASSERT(body.find("git log --oneline") != std::string::npos);
    ASSERT(body.find("git bisect run") != std::string::npos);
    ASSERT(body.find("git revert <sha>") != std::string::npos);
    ASSERT(body.find("git reset --soft") != std::string::npos);
    ASSERT(body.find("git reflog") != std::string::npos);
    ASSERT(body.find("detached worktree") != std::string::npos);
    // The activation budget is 5000 tokens; ~4 chars per token leaves room.
    ASSERT(body.size() < 16000);
}

// A capability whose install fails: the plugin is registered and activatable,
// but the harness refuses the contribution. This is the shape a broken plugin
// takes, and it must leave neither a persisted "on" nor a silent rollback.
class FailingInstallCapability : public Capability {
public:
    std::string name() const override { return "broken"; }
    CapabilityKind kind() const override { return CapabilityKind::Tool; }
    InstallResult install(PluginServices&) override {
        InstallResult r;
        r.error = "synthetic install failure";
        return r;
    }
};

class FailingPlugin : public IPlugin {
public:
    std::string id() const override { return "failing"; }
    std::string version() const override { return "1.0.0"; }
    std::string name() const override { return "Failing probe"; }
    bool initialize(const PluginContext&) override { return true; }
    void shutdown() override {}
    std::vector<std::unique_ptr<Capability>> capabilities() override {
        std::vector<std::unique_ptr<Capability>> caps;
        caps.push_back(std::make_unique<FailingInstallCapability>());
        return caps;
    }
};

std::string read_whole_file(const std::string& path) {
    std::ifstream in(path);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The plugin lifecycle is observable. PluginLoaded/PluginUnloaded were declared
// event types with no fire site — a vocabulary nobody could rely on — and a
// "refresh the list" consumer had to poll instead.
TEST(plugin_lifecycle_events_fire) {
    ScratchConfig scratch("lifecycle-events");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add(std::make_shared<NamedToolPlugin>("probe", "probe_tool"));
    std::vector<std::string> loaded;
    std::vector<std::string> unloaded;
    Events events(runtime.events());
    auto l = events.subscribe<PluginLoadedEvent>(
        [&loaded](const PluginLoadedEvent& e) { loaded.push_back(e.plugin_id); });
    auto u = events.subscribe<PluginUnloadedEvent>(
        [&unloaded](const PluginUnloadedEvent& e) { unloaded.push_back(e.plugin_id); });

    runtime.start(/*use_persisted_state=*/false);
    ASSERT_EQ(loaded.size(), 1u);
    ASSERT_EQ(loaded[0], std::string("probe"));

    ASSERT_TRUE(runtime.set_state("probe", false));
    ASSERT_EQ(unloaded.size(), 1u);
    ASSERT_EQ(unloaded[0], std::string("probe"));

    // Off again is not a second unload: nothing was active to unload.
    ASSERT_TRUE(runtime.set_state("probe", false));
    ASSERT_EQ(unloaded.size(), 1u);

    ASSERT_TRUE(runtime.set_state("probe", true));
    ASSERT_EQ(loaded.size(), 2u);
}

// Failure is part of the lifecycle too, and it carries a reason: a plugin that
// cannot install is rolled back, and the observer learns which one and why.
TEST(plugin_failed_event_carries_the_plugin_and_reason) {
    ScratchConfig scratch("plugin-failed-event");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add(std::make_shared<FailingPlugin>());
    std::vector<PluginFailedEvent> failed;
    Events events(runtime.events());
    auto sub = events.subscribe<PluginFailedEvent>(
        [&failed](const PluginFailedEvent& e) { failed.push_back(e); });

    ASSERT_FALSE(runtime.set_state("failing", true));
    ASSERT_EQ(failed.size(), 1u);
    ASSERT_EQ(failed[0].plugin_id, std::string("failing"));
    ASSERT(failed[0].reason.find("synthetic install failure") != std::string::npos);
}

// An explicit toggle that fails is not persisted as on: the file describes what
// is true, not what was attempted. (A plugin persisted on BEFORE the session is
// a different case -- see the retry test. This one is external, whose default is
// off, so the restored value is unambiguous.)
TEST(failed_toggle_is_not_recorded_as_on) {
    ScratchConfig scratch("failed-toggle");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add(std::make_shared<FailingPlugin>(), /*bundled=*/false);
    ASSERT_FALSE(runtime.set_state("failing", true));
    const std::string text = read_whole_file(scratch.root() + "/amber/plugins/failing/plugin.conf");
    ASSERT(text.find("enabled=0") != std::string::npos);
}

TEST(failed_activation_of_a_persisted_plugin_keeps_it_on) {
    ScratchConfig scratch("failed-retry");
    const std::string dir = scratch.root() + "/amber/plugins/failing";
    fs::create_directories(dir);
    std::ofstream(dir + "/plugin.conf") << "enabled=1\n";

    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add(std::make_shared<FailingPlugin>());
    runtime.start(/*use_persisted_state=*/true);
    // A failed activation at startup is retried next time; it does not silently
    // turn the user's saved choice off.
    ASSERT(read_whole_file(dir + "/plugin.conf").find("enabled=1") != std::string::npos);
}

// A worker that kept the port past deactivation (a source ignoring the
// contract) must find it closed: the add is a no-op, not a registration that
// the ledger can no longer unwind.
TEST(tool_port_add_after_close_is_ignored) {
    ScratchConfig scratch("tool-source-closed");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    auto source = std::make_shared<ToolSourcePlugin>();
    runtime.add(source);
    runtime.start(/*use_persisted_state=*/false);
    ToolPort* port = source->port_;
    ASSERT(port != nullptr);

    ASSERT_TRUE(runtime.set_state("source", false));
    port->add(std::make_unique<StubTool>("late_arrival"));
    ASSERT_FALSE((bool)f.tools.find("late_arrival"));
}

// The bare-host path installs the tool set from the tool plugins' own factories,
// but the list of those factories is hand-written in register_default_tools: a
// new tool plugin missing from it would leave bare hosts (and every test on
// them) without the tool while the runtime has it. This pins the two sets
// together — the drift the shared factories cannot prevent by themselves.
TEST(bare_host_tools_match_the_tool_plugins) {
    ScratchConfig scratch("bare-tool-set");
    Fixture f;
    JobService jobs;
    TodoStore todos;
    SubAgentExecutor subagents;
    CancellationToken cancel;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.attach_host_services(HostServices{&jobs, &todos, &subagents, &cancel});
    for (auto& plugin : make_bundled_plugins()) {
        const std::string id = plugin->id();
        if (id.rfind("tool_", 0) == 0 || id.rfind("search_", 0) == 0)
            runtime.add(plugin); // the tool plugins and the search backends they use
    }
    runtime.start(/*use_persisted_state=*/false);
    std::vector<std::string> via_runtime;
    for (const auto& tool : f.tools.snapshot_tools())
        via_runtime.push_back(tool->name());

    ToolRegistry bare;
    register_default_tools(bare, jobs, todos, cancel);
    std::vector<std::string> via_bare;
    for (const auto& tool : bare.snapshot_tools())
        via_bare.push_back(tool->name());

    const auto joined = [](std::vector<std::string> names) {
        std::sort(names.begin(), names.end());
        std::string out;
        for (const auto& name : names) {
            out += out.empty() ? "" : ",";
            out += name;
        }
        return out;
    };
    ASSERT_EQ(joined(via_runtime), joined(via_bare));
}

// The generation is how a host learns the set moved on without being told — it
// can change from a plugin's worker, where no refresh call site exists.
TEST(registry_generation_moves_on_every_change) {
    ScratchConfig scratch("registry-generation");
    ToolRegistry reg;
    const std::uint64_t empty = reg.generation();
    reg.register_tool(std::make_unique<StubTool>("a"));
    const std::uint64_t added = reg.generation();
    ASSERT(added > empty);

    // A replacement is a change too: the schema the model sees moved on.
    reg.register_tool(std::make_unique<StubTool>("a"));
    const std::uint64_t replaced = reg.generation();
    ASSERT(replaced > added);

    reg.remove_tool("a");
    ASSERT(reg.generation() > replaced);
}

// The mcp plugin owns its command documentation now (the static JSON no longer
// carries the nodes): every verb is documented AND executable, which is the
// guard the completions suite used to provide for the static tree.
TEST(mcp_command_surface_is_documented_and_executable) {
    ScratchConfig scratch("mcp-docs");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(/*use_persisted_state=*/false);

    const CommandRegistry::Entry* mcp = nullptr;
    const CommandRegistry::Entry* prompt = nullptr;
    for (const auto& e : runtime.commands().all()) {
        if (e.spec.name == "mcp")
            mcp = &e;
        if (e.spec.name == "prompt")
            prompt = &e;
    }
    ASSERT_TRUE(mcp != nullptr);
    ASSERT_TRUE(prompt != nullptr);

    ASSERT(!mcp->spec.help.empty());
    ASSERT(!mcp->spec.man.empty());
    for (const char* verb : {"list", "show", "connect", "disconnect", "refresh", "prompts",
                             "enable", "disable", "trust"}) {
        ASSERT_TRUE(mcp->spec.subtree.contains(verb));
        ASSERT_FALSE(mcp->spec.subtree[verb]["help"].get<std::string>().empty());
        ASSERT_FALSE(mcp->spec.subtree[verb]["man"].get<std::string>().empty());
        ASSERT_EQ(mcp->spec.handlers.count(verb), 1u); // documented AND executable
    }
    // The root binds too: `/mcp` with no verb lists the servers.
    ASSERT_EQ(mcp->spec.handlers.count(""), 1u);
    // /prompt takes free-form arguments, so it is a root handler with no
    // children.
    ASSERT_EQ(prompt->spec.handlers.count(""), 1u);
    ASSERT_FALSE(prompt->spec.help.empty());
}

// MCP is a plugin: the client, the tools its servers discover, its command
// namespace, its status segment and its prompt block all arrive through the
// capability path. That is what makes `/set plugin mcp off` a real footprint
// switch — servers are not spawned, discovered tools are gone, and the surfaces
// that describe them leave with them.
TEST(mcp_feature_arrives_and_leaves_as_one_plugin) {
    ScratchConfig scratch("mcp-plugin");
    char buf[4096];
    const std::string cwd = getcwd(buf, sizeof buf) ? buf : ".";
    write_mcp_server_conf(scratch.root(), "echo", "tests/fixtures/mcp_echo", cwd);
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(/*use_persisted_state=*/false);

    // The plugin is bundled and on, and its configured server's discovered tool
    // is in the registry (the fixture speaks the protocol on stdio).
    ASSERT_TRUE(runtime.has("mcp"));
    ASSERT_TRUE(runtime.status("mcp").enabled);
    ASSERT_TRUE((bool)f.tools.find("mcp_echo_echo_tool"));

    // Surfaces: the prompt block, the status segment and the command namespace.
    const std::string mcp_prompt = "# MCP (Model Context Protocol)";
    ASSERT_TRUE(system_blocks_text(runtime).find(mcp_prompt) != std::string::npos);
    ASSERT_TRUE(segment_owned_by(runtime.status(), "mcp"));
    ASSERT_TRUE(command_root_present(runtime.commands(), "mcp"));
    ASSERT_TRUE(command_root_present(runtime.commands(), "prompt"));
    // The segment reads the plugin's own manager: the connected fixture shows
    // as its name (the format the core segment used to render).
    bool segment_text = false;
    for (const auto& s : runtime.status().render(StatusSnapshot{}))
        if (s.id == "mcp")
            segment_text = s.text.find("mcp: echo") != std::string::npos;
    ASSERT_TRUE(segment_text);

    // A tool another owner registered with an MCP-shaped name is not ours.
    f.tools.register_tool(std::make_unique<StubTool>("mcp_echo_someone_elses"), "other_plugin");

    ASSERT_TRUE(runtime.set_state("mcp", false));
    ASSERT_FALSE(runtime.status("mcp").enabled);
    ASSERT_FALSE((bool)f.tools.find("mcp_echo_echo_tool"));
    ASSERT_TRUE((bool)f.tools.find("mcp_echo_someone_elses")); // survives the unwind
    ASSERT_TRUE(system_blocks_text(runtime).find(mcp_prompt) == std::string::npos);
    ASSERT_FALSE(segment_owned_by(runtime.status(), "mcp"));
    ASSERT_FALSE(command_root_present(runtime.commands(), "mcp"));
    ASSERT_FALSE(command_root_present(runtime.commands(), "prompt"));
}

// ---------------------------------------------------------------------------
// The MCP plugin's command surface. The verbs are reached the way the
// dispatcher reaches them — through the command specs the runtime holds — so
// these cover what a user typing /mcp, /prompt or /get mcp actually runs,
// against the real fixture server on stdio.
// ---------------------------------------------------------------------------

namespace {

// Accepts filled input, so /prompt's fill path is exercised rather than its
// print fallback.
class FillUi : public NullUiServices {
public:
    bool fill_input(const std::string& text) override {
        filled = text;
        return true;
    }
    std::string filled;
};

// Call one verb of one command root, as a dispatcher would.
std::string run_verb(PluginRuntime& rt, const std::string& root, const std::string& verb,
                     const std::string& arg) {
    for (const auto& entry : rt.commands().all())
        if (entry.spec.name == root) {
            const auto it = entry.spec.handlers.find(verb);
            return it == entry.spec.handlers.end() ? "<no handler>" : it->second(arg);
        }
    return "<no such root>";
}

// These tests toggle server state, and a toggle persists into the *project*
// mcp dir — which, running from the checkout, is the checkout. Point the
// workspace at a scratch dir for their lifetime.
class ScratchWorkspace {
public:
    explicit ScratchWorkspace(const std::string& tag)
        : dir_(fs::temp_directory_path() / ("amber_ws_" + tag)),
          previous_(Workspace::project_root()) {
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        Workspace::set_root(dir_.string());
    }
    ~ScratchWorkspace() { Workspace::set_root(previous_); }
    ScratchWorkspace(const ScratchWorkspace&) = delete;
    ScratchWorkspace& operator=(const ScratchWorkspace&) = delete;

    // Where the files a project would have live during the test.
    const fs::path& root() const { return dir_; }

private:
    fs::path dir_;
    std::string previous_;
};

// A runtime with one live fixture server configured, exactly as a user would.
struct LiveMcp {
    explicit LiveMcp(const std::string& tag, UiServices* ui = nullptr) : ws(tag), scratch(tag) {
        char buf[4096];
        const std::string cwd = getcwd(buf, sizeof buf) ? buf : ".";
        // Absolute command, cwd inside the (scratch) workspace: the spawn is
        // confined to it, exactly as a real run is.
        write_mcp_server_conf(scratch.root(), "echo", cwd + "/tests/fixtures/mcp_echo", ".");
        if (ui)
            runtime.attach_ui_services(ui);
        runtime.add_bundled();
        runtime.start(/*use_persisted_state=*/false);
    }
    ScratchWorkspace ws;
    ScratchConfig scratch;
    Fixture f;
    PluginRuntime runtime{f.tools, f.cfg, f.ws};
};

} // namespace

// ---------------------------------------------------------------------------
// Project instructions: the project's own AGENTS.md reaches the system prompt.
// ---------------------------------------------------------------------------

namespace {

void write_text(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path);
    f << text;
}

// Just the instructions block, so a nested heading does not have to be guessed
// at from the whole prompt.
std::string instructions_block(PluginRuntime& runtime) {
    const std::string all = system_blocks_text(runtime);
    const std::size_t at = all.find("# Project instructions (AGENTS.md)");
    if (at == std::string::npos)
        return {};
    return all.substr(at, 9 * 1024u);
}

} // namespace

TEST(coding_domain_prose_arrives_and_leaves) {
    ScratchWorkspace ws("coding-domain");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);

    const std::string blocks = system_blocks_text(runtime);
    ASSERT(blocks.find("## Version control (git)") != std::string::npos);
    ASSERT(blocks.find("## Engineering discipline") != std::string::npos);
    ASSERT(blocks.find("Open for extension") != std::string::npos);

    // Switching the domain off removes both blocks; a generic block stays.
    ASSERT_TRUE(runtime.set_state("coding", false));
    const std::string after = system_blocks_text(runtime);
    ASSERT(after.find("## Version control (git)") == std::string::npos);
    ASSERT(after.find("## Engineering discipline") == std::string::npos);
    ASSERT(after.find("# Skills") != std::string::npos);
}

TEST(project_instructions_reach_the_system_prompt) {
    ScratchWorkspace ws("instructions");
    write_text(ws.root() / "AGENTS.md",
               "# Project\n\nBuild with `make test`. Generated files under build/ are not "
               "hand-edited.\n");
    write_text(ws.root() / "apps" / "web" / "AGENTS.md", "# Web\n\nNext.js app.\n");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);

    const std::string block = instructions_block(runtime);
    ASSERT(!block.empty());
    ASSERT(block.find("Build with `make test`") != std::string::npos);
    // The convention's precedence, stated where the agent reads it.
    ASSERT(block.find("outrank this system prompt") != std::string::npos);
    // Nested files are named, not inlined: the nearest one wins, and the agent
    // reads the one that applies to the tree it is working in.
    ASSERT(block.find("apps/web/AGENTS.md") != std::string::npos);
    ASSERT(block.find("Next.js app") == std::string::npos);

    // Disabling the plugin takes the block with it.
    ASSERT_TRUE(runtime.set_state("instructions", false));
    ASSERT(instructions_block(runtime).empty());
}

TEST(project_instructions_are_capped_and_say_so) {
    ScratchWorkspace ws("instructions-cap");
    std::string big = "# Project\n";
    for (int i = 0; i < 4000; ++i)
        big += "line " + std::to_string(i) + ": the conventions of this project, at length.\n";
    write_text(ws.root() / "AGENTS.md", big);
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);

    const std::string block = instructions_block(runtime);
    ASSERT(!block.empty());
    ASSERT(block.find("[truncated at 8 KB") != std::string::npos);
    ASSERT(block.size() <= 9 * 1024u);
    // The cut lands on a line boundary: what the agent was given is
    // trustworthy, and what it was not given is not silently missing.
    ASSERT(block.find("line 0:") != std::string::npos);
    ASSERT(block.find("line 1000:") == std::string::npos);
}

// The 8 KB cut used to end at a marker with no map of what was omitted. The
// digest names every top-level section past the cut with its file line range;
// a section whose heading is inside the kept body is not listed again.
TEST(project_instructions_digest_names_omitted_sections) {
    ScratchWorkspace ws("instructions-digest");
    std::string big = "# Project\n## Early section\nearly\n";
    for (int i = 0; i < 2000; ++i)
        big += "filler " + std::to_string(i) + "\n";
    big += "\n## First late section\nbody one\n\n## Second late section\nbody two\n";
    write_text(ws.root() / "AGENTS.md", big);
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);

    const std::string block = instructions_block(runtime);
    ASSERT(!block.empty());
    ASSERT(block.find("[truncated at 8 KB") != std::string::npos);
    // The heading is at line 2005; its section ends at the line before the next
    // heading (2008), trailing blank lines trimmed -> 2006.
    ASSERT(block.find("- First late section (lines 2005-2006)") != std::string::npos);
    ASSERT(block.find("- Second late section (lines 2008-2009)") != std::string::npos);
    // Early is visible in the body, so it is not an omitted section.
    ASSERT(block.find("## Early section") != std::string::npos);
    ASSERT(block.find("- Early section") == std::string::npos);
}

TEST(project_instructions_absent_leaves_no_block) {
    ScratchWorkspace ws("instructions-none");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);
    ASSERT(instructions_block(runtime).empty());
}

// In a repository the scan asks git: tracked plus untracked-not-ignored, and git
// does not descend into a nested repository or linked worktree. That is what
// keeps a gitignored tree and a developer's sibling checkout out of the prompt.
TEST(project_instructions_git_scan_skips_ignored_and_nested_repositories) {
    ScratchWorkspace ws("instructions-git");
    ASSERT_EQ(std::system(("git -C '" + ws.root().string() + "' init -q").c_str()), 0);
    write_text(ws.root() / "AGENTS.md", "# Project\n\nrules\n");
    write_text(ws.root() / "pkg" / "AGENTS.md", "# Pkg\n\nrules\n");
    write_text(ws.root() / ".gitignore", "ignored/\n");
    write_text(ws.root() / "ignored" / "AGENTS.md", "# Ignored\n\nrules\n");
    // A linked worktree root is a directory with a `.git` FILE.
    write_text(ws.root() / "sibling" / ".git", "gitdir: /tmp/elsewhere\n");
    write_text(ws.root() / "sibling" / "AGENTS.md", "# Sibling\n\nrules\n");

    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);

    const std::string block = instructions_block(runtime);
    ASSERT(block.find("pkg/AGENTS.md") != std::string::npos);
    ASSERT(block.find("ignored/AGENTS.md") == std::string::npos);
    ASSERT(block.find("sibling/AGENTS.md") == std::string::npos);
}

// Without git the fallback walk still refuses to enter a nested repository: a
// directory carrying `.git` is a different tree, whatever its ignore rules are.
TEST(project_instructions_walk_skips_nested_repositories) {
    ScratchWorkspace ws("instructions-walk");
    write_text(ws.root() / "AGENTS.md", "# Project\n\nrules\n");
    write_text(ws.root() / "pkg" / "AGENTS.md", "# Pkg\n\nrules\n");
    write_text(ws.root() / "sibling" / ".git", "gitdir: /tmp/elsewhere\n");
    write_text(ws.root() / "sibling" / "AGENTS.md", "# Sibling\n\nrules\n");

    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);

    const std::string block = instructions_block(runtime);
    ASSERT(block.find("pkg/AGENTS.md") != std::string::npos);
    ASSERT(block.find("sibling/AGENTS.md") == std::string::npos);
}

TEST(project_instructions_report_names_the_file_and_nested) {
    ScratchWorkspace ws("instructions-report");
    write_text(ws.root() / "AGENTS.md", "# Project\n\nrules\n");
    write_text(ws.root() / "pkg" / "AGENTS.md", "# Pkg\n\nrules\n");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);

    auto* plugin = dynamic_cast<plugins::ProjectInstructionsPlugin*>(runtime.find("instructions"));
    ASSERT(plugin != nullptr);
    const auto lines = plugin->report_lines();
    ASSERT(!lines.empty());
    ASSERT(lines.front().find("AGENTS.md") != std::string::npos);
    ASSERT(lines.front().find("bytes") != std::string::npos);
    bool named_nested = false;
    for (const auto& line : lines)
        if (line.find("nested: pkg/AGENTS.md") != std::string::npos)
            named_nested = true;
    ASSERT(named_nested);
}

// ---------------------------------------------------------------------------
// Project index: amber's own notes about the project, as a session-fixed block.
// ---------------------------------------------------------------------------

namespace {

// A scratch state root. The index lives in the project's private state, so a
// test that writes one must not write into the developer's real tree.
class ScratchState {
public:
    explicit ScratchState(const std::string& tag)
        : dir_(fs::temp_directory_path() / ("amber_state_" + tag)) {
        fs::remove_all(dir_);
        fs::create_directories(dir_);
        const char* saved = std::getenv("XDG_STATE_HOME");
        had_ = saved != nullptr;
        if (had_)
            saved_ = saved;
        setenv("XDG_STATE_HOME", dir_.c_str(), 1);
    }
    ~ScratchState() {
        if (had_)
            setenv("XDG_STATE_HOME", saved_.c_str(), 1);
        else
            unsetenv("XDG_STATE_HOME");
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    ScratchState(const ScratchState&) = delete;
    ScratchState& operator=(const ScratchState&) = delete;

private:
    fs::path dir_;
    std::string saved_;
    bool had_ = false;
};

// The index file a user would have, at the path the plugin reads.
void write_project_index(const std::string& text) {
    write_text(fs::path(Workspace::state_dir()) / "project-index.md", text);
}

// Just the index block, so a neighbouring block's headings are not mistaken for
// its own.
std::string project_index_block(PluginRuntime& runtime) {
    const std::string all = system_blocks_text(runtime);
    const std::size_t at = all.find("# Project index");
    if (at == std::string::npos)
        return {};
    return all.substr(at, 5 * 1024u);
}

} // namespace

TEST(project_index_reaches_the_system_prompt) {
    ScratchWorkspace ws("index");
    ScratchState state("index");
    write_project_index("# Project: turbo-shop\n\nmonorepo, backend .NET 8, Angular web\n");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);

    const std::string block = project_index_block(runtime);
    ASSERT(!block.empty());
    ASSERT(block.find("turbo-shop") != std::string::npos);
    // Precedence, stated where the agent reads it: the project's own file wins.
    ASSERT(block.find("AGENTS.md") != std::string::npos);

    // Disabling the plugin takes the block with it, and nothing else.
    ASSERT_TRUE(runtime.set_state("project_index", false));
    ASSERT(project_index_block(runtime).empty());
}

TEST(project_index_is_frozen_within_a_session) {
    ScratchWorkspace ws("index-frozen");
    ScratchState state("index-frozen");
    write_project_index("# Project: one\n");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);

    const std::string first = project_index_block(runtime);
    ASSERT(first.find("Project: one") != std::string::npos);
    // The file changes on disk mid-session; the block does not, because the
    // system prefix is what the server caches.
    write_project_index("# Project: two\n");
    ASSERT_EQ(project_index_block(runtime), first);

    // An explicit refresh is the one thing that moves it.
    auto* plugin = dynamic_cast<plugins::ProjectIndexPlugin*>(runtime.find("project_index"));
    ASSERT(plugin != nullptr);
    plugin->refresh();
    ASSERT(project_index_block(runtime).find("Project: two") != std::string::npos);
}

TEST(project_index_provenance_reports_a_moved_source) {
    ScratchWorkspace ws("index-stale");
    ScratchState state("index-stale");
    write_text(ws.root() / "docker-compose.yml", "services:\n  api:\n");
    const std::string hash =
        plugins::ProjectIndexPlugin::hash_file((ws.root() / "docker-compose.yml").string());
    ASSERT(!hash.empty());
    write_project_index("# Project: compose-app\n\nsources: docker-compose.yml@" + hash +
                        "\n\nruns in compose\n");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);

    const std::string fresh = project_index_block(runtime);
    ASSERT(fresh.find("compose-app") != std::string::npos);
    ASSERT(fresh.find("changed since") == std::string::npos);

    // The source moves; the recorded hash no longer matches, and the block says
    // so rather than presenting facts that may be out of date as current.
    write_text(ws.root() / "docker-compose.yml", "services:\n  api:\n  db:\n");
    auto* plugin = dynamic_cast<plugins::ProjectIndexPlugin*>(runtime.find("project_index"));
    ASSERT(plugin != nullptr);
    plugin->refresh();
    const std::string block = project_index_block(runtime);
    ASSERT(block.find("changed since") != std::string::npos);
    ASSERT(block.find("docker-compose.yml@" + hash) != std::string::npos);
}

TEST(project_index_absent_leaves_no_block) {
    ScratchWorkspace ws("index-none");
    ScratchState state("index-none");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);
    ASSERT(project_index_block(runtime).empty());
}

TEST(project_index_is_capped_and_says_so) {
    ScratchWorkspace ws("index-cap");
    ScratchState state("index-cap");
    std::string big = "# Project: big\n";
    for (int i = 0; i < 400; ++i)
        big += "line " + std::to_string(i) + ": a fact about this project, at length.\n";
    write_project_index(big);
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);

    const std::string block = project_index_block(runtime);
    ASSERT(block.find("[truncated at 4 KB") != std::string::npos);
    ASSERT(block.find("line 0:") != std::string::npos);
    ASSERT(block.find("line 300:") == std::string::npos);
}

// ---------------------------------------------------------------------------
// Project index tools: init/add/update/remove/show, the redaction rule, the
// command surface, and the frozen block they deliberately do not move.
// ---------------------------------------------------------------------------

namespace {

// Execute a registered tool the way the agent loop does.
ToolResult call_tool(Fixture& f, const std::string& name, const json& args) {
    const std::shared_ptr<Tool> tool = f.tools.find(name);
    if (!tool)
        return ToolResult{false, {}, "no such tool: " + name, nullptr};
    return tool->execute(args);
}

std::string store_text() {
    return plugins::index_store::read_all();
}

} // namespace

TEST(index_init_proposes_and_commits) {
    ScratchWorkspace ws("index-init");
    ScratchState state("index-init");
    write_text(ws.root() / "docker-compose.yml", "services:\n  api:\n");
    write_text(ws.root() / "package.json",
               R"({"name":"web","dependencies":{"next":"14.0.0","react":"18.0.0"},)"
               R"("devDependencies":{"vitest":"1.0.0"}})");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);

    const std::shared_ptr<Tool> init = f.tools.find("index_init");
    ASSERT(init != nullptr);
    // The proposal is free; the write is the one that asks for approval.
    ASSERT_FALSE(init->requires_approval(json::object()));
    ASSERT_TRUE(init->requires_approval(json{{"commit", "body"}}));

    const ToolResult proposed = call_tool(f, "index_init", json::object());
    ASSERT(proposed.ok);
    ASSERT(proposed.output.find("#dockercompose") != std::string::npos);
    ASSERT(proposed.output.find("#next") != std::string::npos);
    ASSERT(proposed.output.find("#vitest") != std::string::npos);
    // The proposal names what it read, so a disagreement is checkable.
    ASSERT(proposed.output.find("sources:") != std::string::npos);
    // The redaction/confirm guidance is an advisory outside the body, not an
    // entry a verbatim copy would persist as a fact.
    ASSERT(proposed.output.find("[index_init advisory") != std::string::npos);
    ASSERT(proposed.output.find("artefacts: record keys") == std::string::npos);
    const size_t close = proposed.output.find("[/index_init advisory]");
    ASSERT(close != std::string::npos);
    ASSERT(proposed.output.substr(close).find("record keys, tokens and hosts") ==
           std::string::npos);
    // Nothing is written until the confirmed body comes back.
    ASSERT(!fs::exists(plugins::ProjectIndexPlugin::file_path()));

    ASSERT(!project_index_block(runtime).empty() == false);
    const ToolResult committed = call_tool(f, "index_init", json{{"commit", proposed.output}});
    ASSERT(committed.ok);
    ASSERT(fs::exists(plugins::ProjectIndexPlugin::file_path()));
    // Copying the whole proposal verbatim must not persist the advisory: the
    // writer strips it, so the index holds entries only.
    std::ifstream index_file(plugins::ProjectIndexPlugin::file_path());
    const std::string written((std::istreambuf_iterator<char>(index_file)),
                              std::istreambuf_iterator<char>());
    ASSERT(written.find("index_init advisory") == std::string::npos);
    ASSERT(written.find("record keys, tokens and hosts") == std::string::npos);
    ASSERT(written.find("Confirm this with the user") == std::string::npos);
    ASSERT(written.find("#dockercompose") != std::string::npos);
    // The running session's block is frozen: it moves on refresh, and would
    // have moved by itself at the next session's activation.
    auto* plugin = dynamic_cast<plugins::ProjectIndexPlugin*>(runtime.find("project_index"));
    ASSERT(plugin != nullptr);
    plugin->refresh();
    ASSERT(project_index_block(runtime).find("dockercompose") != std::string::npos);
}

TEST(index_init_refuses_a_credential) {
    ScratchWorkspace ws("index-init-secret");
    ScratchState state("index-init-secret");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);

    const std::string body =
        "# Project: x\n\n## Where things are\nADO_PAT=ghp_0123456789012345678901234567890123\n";
    const ToolResult r = call_tool(f, "index_init", json{{"commit", body}});
    ASSERT_FALSE(r.ok);
    ASSERT(r.error.find("pointer") != std::string::npos);
    ASSERT(!fs::exists(plugins::ProjectIndexPlugin::file_path()));
}

TEST(index_add_writes_and_creates_what_is_missing) {
    ScratchWorkspace ws("index-add");
    ScratchState state("index-add");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);
    // Render once, so the frozen block is the empty one this session sees.
    ASSERT(project_index_block(runtime).empty());

    const ToolResult added =
        call_tool(f, "index_add", json{{"section", "shape"}, {"entry", "monorepo, two services"}});
    ASSERT(added.ok);
    // The tool returns the canonical entry it wrote: the agent's view of a
    // mid-session change comes from the result, not from the prompt.
    ASSERT(added.output.find("monorepo, two services") != std::string::npos);
    const std::string body = store_text();
    ASSERT(body.find("# Project:") != std::string::npos);
    ASSERT(body.find("## Shape") != std::string::npos);
    ASSERT(body.find("monorepo, two services") != std::string::npos);

    // The block has not moved; a new session (or a refresh) reads the file.
    ASSERT(project_index_block(runtime).empty());

    // A second add lands under the same heading rather than repeating it.
    ASSERT(call_tool(f, "index_add", json{{"section", "shape"}, {"entry", "postgres + redis"}}).ok);
    const std::string two = store_text();
    ASSERT(two.find("## Shape") == two.rfind("## Shape"));
    ASSERT(two.find("postgres + redis") != std::string::npos);

    // An unknown section is used as the heading it was given.
    ASSERT(call_tool(f, "index_add", json{{"section", "Notes"}, {"entry", "hand-written"}}).ok);
    ASSERT(store_text().find("## Notes") != std::string::npos);

    // A later section does not capture an earlier one's entries: a new "shape"
    // line lands inside Shape, before whatever heading follows it.
    ASSERT(call_tool(f, "index_add", json{{"section", "runs"}, {"entry", "up: make dev"}}).ok);
    ASSERT(call_tool(f, "index_add", json{{"section", "shape"}, {"entry", "third fact"}}).ok);
    const std::string ordered = store_text();
    ASSERT(ordered.find("## Shape") < ordered.find("third fact"));
    ASSERT(ordered.find("third fact") < ordered.find("## Notes"));

    // An entry with nothing in it is refused, not written as a blank line.
    const ToolResult empty =
        call_tool(f, "index_add", json{{"section", "shape"}, {"entry", "   "}});
    ASSERT_FALSE(empty.ok);

    // A file a hand edit left without a trailing newline still gains its
    // headings cleanly.
    write_text(fs::path(plugins::ProjectIndexPlugin::file_path()), "# Project: hand-edited");
    ASSERT(
        call_tool(f, "index_add", json{{"section", "shape"}, {"entry", "after a hand edit"}}).ok);
    ASSERT(store_text().find("\n## Shape\nafter a hand edit\n") != std::string::npos);

    // Every mutation is in the log, next to the file.
    std::ifstream log(plugins::index_store::log_path());
    std::stringstream logged;
    logged << log.rdbuf();
    ASSERT(logged.str().find("shape") != std::string::npos);
}

TEST(project_index_block_follows_a_compaction) {
    ScratchWorkspace ws("index-compaction");
    ScratchState state("index-compaction");
    write_project_index("# Project: before\n");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);
    ASSERT(project_index_block(runtime).find("Project: before") != std::string::npos);

    write_project_index("# Project: after\n");

    // A compression that failed leaves the context where it was, so the prefix
    // must not move either.
    CompressionCompletedEvent failed;
    failed.success = false;
    Events(runtime.events()).publish(failed);
    ASSERT(project_index_block(runtime).find("Project: before") != std::string::npos);

    // A compression that happened rebuilds the context anyway: the block is
    // re-read there, and nowhere else mid-session.
    CompressionCompletedEvent done;
    done.success = true;
    Events(runtime.events()).publish(done);
    ASSERT(project_index_block(runtime).find("Project: after") != std::string::npos);

    // Switching the plugin off releases the subscription with it.
    ASSERT_TRUE(runtime.set_state("project_index", false));
    ASSERT_FALSE(runtime.events().has_subscribers(EventType::CompressionCompleted));
}

TEST(project_index_does_not_read_outside_the_workspace) {
    ScratchWorkspace ws("index-confine");
    ScratchState state("index-confine");
    // A file outside the workspace, whose hash is recorded faithfully in the
    // index. The provenance check exists to notice changes to the project's own
    // files; a source that climbs out of the workspace is not one of those, and
    // reading it would make the index a way to hash arbitrary paths.
    const fs::path outside = fs::temp_directory_path() / "amber_outside_source.yml";
    write_text(outside, "services:\n  api:\n");
    const std::string hash = plugins::ProjectIndexPlugin::hash_file(outside.string());
    write_project_index("# Project: x\n\nsources: ../amber_outside_source.yml@" + hash +
                        "\n\nruns in compose\n");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);

    // Refused rather than read: "cannot check" is not "fine", so the block says
    // the fact may be stale.
    ASSERT(project_index_block(runtime).find("changed since") != std::string::npos);
    std::error_code ec;
    fs::remove(outside, ec);
}

TEST(index_add_refuses_a_credential) {
    ScratchWorkspace ws("index-add-secret");
    ScratchState state("index-add-secret");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);

    const ToolResult r = call_tool(
        f, "index_add",
        json{{"section", "where"}, {"entry", "api_key = \"sk-abcdefghijklmnopqrstuvwxyz\""}});
    ASSERT_FALSE(r.ok);
    ASSERT(r.error.find("pointer") != std::string::npos);
    ASSERT(store_text().empty());
}

TEST(index_update_replaces_a_line_and_reports_a_miss) {
    ScratchWorkspace ws("index-update");
    ScratchState state("index-update");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);
    ASSERT(call_tool(f, "index_add", json{{"section", "runs"}, {"entry", "up: make dev"}}).ok);

    const ToolResult updated = call_tool(
        f, "index_update", json{{"find", "make dev"}, {"entry", "up: docker compose up -d"}});
    ASSERT(updated.ok);
    ASSERT(store_text().find("docker compose up -d") != std::string::npos);
    ASSERT(store_text().find("make dev") == std::string::npos);

    // A find that matches nothing is reported, not silently ignored.
    const ToolResult missed = call_tool(f, "index_update", json{{"find", "nope"}, {"entry", "x"}});
    ASSERT_FALSE(missed.ok);
    ASSERT(missed.error.find("no line") != std::string::npos);

    // A blank find or a blank replacement is a usage error, not a match on
    // every line.
    ASSERT_FALSE(call_tool(f, "index_update", json{{"find", " "}, {"entry", "x"}}).ok);
    ASSERT_FALSE(call_tool(f, "index_update", json{{"find", "make"}, {"entry", " "}}).ok);
    ASSERT_FALSE(call_tool(f, "index_remove", json{{"match", ""}}).ok);
}

TEST(index_store_refuses_an_empty_body) {
    ScratchWorkspace ws("index-empty-body");
    ScratchState state("index-empty-body");
    std::string error;
    ASSERT_FALSE(plugins::index_store::write_all("   \n", error));
    ASSERT(error.find("empty") != std::string::npos);
}

TEST(index_init_scans_every_stack_it_knows) {
    ScratchWorkspace ws("index-scan");
    ScratchState state("index-scan");
    write_text(ws.root() / "Makefile", "all:\n\t@true\n");
    write_text(ws.root() / "CMakeLists.txt", "project(x)\n");
    write_text(ws.root() / "Cargo.toml", "[package]\nname = \"x\"\n");
    write_text(ws.root() / "go.mod", "module example.com/x\n");
    write_text(ws.root() / "package.json", R"({"dependencies":{"@angular/core":"17"},)"
                                           R"("devDependencies":{"typescript":"5"}})");
    write_text(ws.root() / "tsconfig.json", "{}\n");
    write_text(ws.root() / "composer.json", R"({"require":{"laravel/framework":"11"}})");
    write_text(ws.root() / "requirements.txt", "django\npytest\n");
    write_text(ws.root() / ".env.example", "KEY=\n");
    write_text(ws.root() / ".github" / "workflows" / "ci.yml", "on: push\n");
    write_text(ws.root() / "app.csproj",
               "<Project><PropertyGroup><TargetFramework>net8.0</TargetFramework></PropertyGroup>"
               "<ItemGroup><PackageReference Include=\"Microsoft.EntityFrameworkCore\" />"
               "</ItemGroup></Project>");
    write_text(ws.root() / "old.csproj", "<Project></Project>");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);

    const ToolResult proposed = call_tool(f, "index_init", json::object());
    ASSERT(proposed.ok);
    for (const char* tag : {"#angular", "#typescript", "#laravel", "#php", "#python", "#django",
                            "#pytest", "#rust", "#go", "#cmake", "#dotnet", "#efcore"})
        ASSERT(proposed.output.find(tag) != std::string::npos);
    ASSERT(proposed.output.find("net8.0") != std::string::npos);
    ASSERT(proposed.output.find("GitHub Actions") != std::string::npos);
    ASSERT(proposed.output.find(".env.example") != std::string::npos);
    ASSERT(proposed.output.find("build: make") != std::string::npos);
}

TEST(index_init_reads_a_manifest_that_is_not_json) {
    ScratchWorkspace ws("index-bad-manifest");
    ScratchState state("index-bad-manifest");
    write_text(ws.root() / "package.json", "this is not json\n");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);

    const ToolResult proposed = call_tool(f, "index_init", json::object());
    ASSERT(proposed.ok);
    // The file is still named as a source even when its contents are unreadable.
    ASSERT(proposed.output.find("#node") != std::string::npos);
    ASSERT(proposed.output.find("package.json@") != std::string::npos);
}

TEST(index_remove_takes_a_line_out_and_reports_a_miss) {
    ScratchWorkspace ws("index-remove");
    ScratchState state("index-remove");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);
    ASSERT(call_tool(f, "index_add", json{{"section", "shape"}, {"entry", "one service"}}).ok);
    ASSERT(call_tool(f, "index_add", json{{"section", "shape"}, {"entry", "two services"}}).ok);

    const ToolResult removed = call_tool(f, "index_remove", json{{"match", "one service"}});
    ASSERT(removed.ok);
    ASSERT(store_text().find("one service") == std::string::npos);
    ASSERT(store_text().find("two services") != std::string::npos);

    const ToolResult missed = call_tool(f, "index_remove", json{{"match", "gone"}});
    ASSERT_FALSE(missed.ok);
    ASSERT(missed.error.find("no line") != std::string::npos);
}

TEST(index_show_prints_the_file_or_says_there_is_none) {
    ScratchWorkspace ws("index-show");
    ScratchState state("index-show");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);

    const std::shared_ptr<Tool> show = f.tools.find("index_show");
    ASSERT(show != nullptr);
    ASSERT_TRUE(show->is_read_only());

    const ToolResult none = call_tool(f, "index_show", json::object());
    ASSERT(none.ok);
    ASSERT(none.output.find("no project index") != std::string::npos);

    ASSERT(call_tool(f, "index_add", json{{"section", "shape"}, {"entry", "a monorepo"}}).ok);
    const ToolResult some = call_tool(f, "index_show", json::object());
    ASSERT(some.ok);
    ASSERT(some.output.find("a monorepo") != std::string::npos);
}

TEST(index_command_shows_the_file_and_refreshes_the_block) {
    ScratchWorkspace ws("index-cmd");
    ScratchState state("index-cmd");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);

    ASSERT(run_verb(runtime, "index", "show", "").find("no project index") != std::string::npos);
    ASSERT(project_index_block(runtime).empty());
    ASSERT(call_tool(f, "index_add", json{{"section", "shape"}, {"entry", "angular + dotnet"}}).ok);
    ASSERT(run_verb(runtime, "index", "show", "").find("angular") != std::string::npos);
    // Still frozen, and the explicit refresh is what moves it.
    ASSERT(project_index_block(runtime).empty());
    ASSERT(run_verb(runtime, "index", "refresh", "").find("index re-read") != std::string::npos);
    ASSERT(project_index_block(runtime).find("angular") != std::string::npos);
}

TEST(index_tools_travel_with_the_plugin) {
    ScratchWorkspace ws("index-tools-travel");
    ScratchState state("index-tools-travel");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);
    ASSERT(f.tools.find("index_show") != nullptr);

    // Switching the index off takes its tools and its command with it: a writer
    // nothing reads is exactly the half-state the ledger exists to prevent.
    ASSERT_TRUE(runtime.set_state("project_index", false));
    ASSERT(f.tools.find("index_show") == nullptr);
    bool command_gone = true;
    for (const auto& entry : runtime.commands().all())
        if (entry.spec.name == "index")
            command_gone = false;
    ASSERT_TRUE(command_gone);
}

// ---------------------------------------------------------------------------
// The coding library: the project's tags select entries, one trigger line each,
// as a per-turn block the domain owns.
// ---------------------------------------------------------------------------

namespace {

std::string head_blocks_text(PluginRuntime& runtime) {
    std::string all;
    for (auto& text : runtime.prompts().render_all(PromptPlacement::Head))
        all += text;
    return all;
}

} // namespace

TEST(library_entry_header_is_parsed) {
    const plugins::LibraryEntry entry = plugins::parse_library_entry(
        "library/x.md", "name: x\ntrigger: when something applies\ntags: #cpp, "
                        "#testing\nweight: 90\nreference: docs\n\nbody\n");
    ASSERT_EQ(entry.trigger, std::string("when something applies"));
    ASSERT_EQ(entry.tags.size(), 2u);
    ASSERT(entry.tags[0] == "cpp");
    ASSERT(entry.tags[1] == "testing");
    ASSERT(plugins::entry_matches(entry, {"testing"}));
    ASSERT_FALSE(plugins::entry_matches(entry, {"python"}));

    // An entry with no trigger has nothing to render, a header that does not
    // lead the file is a body line, and an entry with no tags matches nothing.
    ASSERT(plugins::parse_library_entry("library/y.md", "just a body\n").trigger.empty());
    ASSERT(plugins::parse_library_entry("library/z.md", "body first\ntrigger: too late\n")
               .trigger.empty());
    ASSERT_FALSE(plugins::entry_matches(plugins::LibraryEntry{}, {"cpp"}));

    // Weight decides what survives the cap: a missing weight is the default, a
    // malformed one is the default rather than a hidden entry, and the ceiling
    // is 100.
    ASSERT_EQ(entry.weight, 90);
    ASSERT_EQ(plugins::parse_library_entry("library/a.md", "trigger: t\n").weight, 50);
    ASSERT_EQ(plugins::parse_library_entry("library/b.md", "weight: heavy\ntrigger: t\n").weight,
              50);
    ASSERT_EQ(plugins::parse_library_entry("library/c.md", "weight: 800\ntrigger: t\n").weight,
              100);
}

TEST(library_ranking_puts_the_heaviest_first) {
    ScratchWorkspace ws("library-rank");
    ScratchState state("library-rank");
    write_project_index("# Project: x\n\n## Library selection\n#cpp\n");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);

    const std::string head = head_blocks_text(runtime);
    // Weight, not the filesystem's order: the topic outranks the entries, and
    // each entry outranks a lighter one.
    const std::size_t topic = head.find("which layer to reach for first");
    const std::size_t iterator = head.find("still holds an iterator");
    const std::size_t ownership = head.find("say who owns it");
    const std::size_t concurrency = head.find("async task outlives");
    const std::size_t dependencies = head.find("a build must work somewhere else");
    const std::size_t build_loop = head.find("does not document how");
    ASSERT(topic != std::string::npos);
    ASSERT(topic < iterator);
    ASSERT(iterator < ownership);
    ASSERT(ownership < concurrency);
    ASSERT(concurrency < dependencies);
    ASSERT(dependencies < build_loop);
}

TEST(library_topic_is_one_line_and_hides_its_children) {
    ScratchWorkspace ws("library-topic");
    ScratchState state("library-topic");
    write_project_index("# Project: x\n\n## Library selection\n#cpp #debugging\n");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);

    const std::string head = head_blocks_text(runtime);
    ASSERT(head.find("which layer to reach for first (3 entries)") != std::string::npos);
    ASSERT(head.find("library_lookup(\"cpp-debugging\")") != std::string::npos);
    // One line stands for three entries: the children cost nothing until they
    // are asked for.
    ASSERT(head.find("sanitizer") == std::string::npos);
    ASSERT(head.find("a debugger answers the question") == std::string::npos);
    ASSERT(head.find("cheapest reviewer") == std::string::npos);
}

TEST(index_selection_tags_are_read_from_the_section) {
    const std::vector<std::string> tags = plugins::selected_tags_from(
        "# Project: x\n\n## Shape\nsomething\n\n## Library selection\n#cpp #docker, #testing\n");
    ASSERT_EQ(tags.size(), 3u);
    ASSERT(tags[0] == "cpp");
    ASSERT(tags[1] == "docker");
    ASSERT(tags[2] == "testing");
    ASSERT(plugins::selected_tags_from("# Project: x\n").empty());
}

TEST(coding_library_index_lists_what_the_project_selects) {
    ScratchWorkspace ws("library-select");
    ScratchState state("library-select");
    write_project_index("# Project: x\n\n## Library selection\n#cpp #testing\n");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);

    const std::string head = head_blocks_text(runtime);
    ASSERT(head.find("Library entries this project selects") != std::string::npos);
    ASSERT(head.find("when a container is mutated") != std::string::npos);
    ASSERT(head.find("library_lookup(\"cpp-iterator-invalidation\")") != std::string::npos);
    // None of it is in the system prefix: the selection is per-turn.
    ASSERT(system_blocks_text(runtime).find("Library entries this project selects") ==
           std::string::npos);
    // A handful of lines, not a chapter: the cap counts entries, so the count is
    // of entry lines. The block's own header is the same three lines every turn.
    // With more entries than the cap, the heaviest survive it -- which the
    // ranking test above pins by order.
    std::size_t entries = 0;
    std::istringstream rendered(head);
    std::string line;
    while (std::getline(rendered, line))
        if (line.rfind("- ", 0) == 0)
            ++entries;
    ASSERT(entries > 0u);
    ASSERT(entries <= 25u);
}

TEST(coding_library_index_stays_dark_without_a_selection) {
    ScratchWorkspace ws("library-dark");
    ScratchState state("library-dark");
    write_project_index("# Project: x\n\n## Shape\nno tags yet\n");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);
    ASSERT(head_blocks_text(runtime).find("Library entries") == std::string::npos);

    // A tag no entry carries selects nothing. The tags are read per turn, so
    // the change needs no refresh.
    write_project_index("# Project: x\n\n## Library selection\n#cobol\n");
    ASSERT(head_blocks_text(runtime).find("Library entries") == std::string::npos);
}

TEST(coding_library_index_leaves_with_the_domain) {
    ScratchWorkspace ws("library-domain");
    ScratchState state("library-domain");
    write_project_index("# Project: x\n\n## Library selection\n#cpp\n");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);
    ASSERT(head_blocks_text(runtime).find("Library entries") != std::string::npos);
    ASSERT_TRUE(runtime.set_state("coding", false));
    ASSERT(head_blocks_text(runtime).find("Library entries") == std::string::npos);
}

TEST(library_lookup_reads_an_entry_by_name) {
    ScratchWorkspace ws("library-lookup-name");
    ScratchState state("library-lookup-name");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);

    const ToolResult entry =
        call_tool(f, "library_lookup", json{{"name", "cpp-iterator-invalidation"}});
    ASSERT(entry.ok);
    ASSERT(entry.output.find("std::vector") != std::string::npos);

    // A topic reads as its own text plus the entries inside it.
    const ToolResult topic = call_tool(f, "library_lookup", json{{"name", "cpp-debugging"}});
    ASSERT(topic.ok);
    ASSERT(topic.output.find("Entries in this topic") != std::string::npos);
    ASSERT(topic.output.find("sanitizers:") != std::string::npos);
    ASSERT(topic.output.find("debugger:") != std::string::npos);

    // And a child is reachable by name: the topic's "(3 entries)" is a promise
    // the tool keeps.
    const ToolResult child = call_tool(f, "library_lookup", json{{"name", "sanitizers"}});
    ASSERT(child.ok);
    ASSERT(child.output.find("heap-use-after-free") != std::string::npos);
}

TEST(library_lookup_answers_a_query_and_reports_a_miss) {
    ScratchWorkspace ws("library-lookup-query");
    ScratchState state("library-lookup-query");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);

    const ToolResult hits = call_tool(f, "library_lookup", json{{"query", "sanitizer"}});
    ASSERT(hits.ok);
    ASSERT(hits.output.find("sanitizers:") != std::string::npos);
    ASSERT(hits.output.find("Ask for one by name") != std::string::npos);

    const ToolResult miss = call_tool(f, "library_lookup", json{{"query", "cobol"}});
    ASSERT(miss.ok);
    ASSERT(miss.output.find("nothing in the library matches") != std::string::npos);

    const ToolResult unknown = call_tool(f, "library_lookup", json{{"name", "cobol"}});
    ASSERT_FALSE(unknown.ok);
    ASSERT(unknown.error.find("cobol") != std::string::npos);

    // No arguments at all is usage, not a crash and not silence.
    const ToolResult usage = call_tool(f, "library_lookup", json::object());
    ASSERT(usage.ok);
    ASSERT(usage.output.find("needs a `name`") != std::string::npos);
}

TEST(library_lookup_travels_with_the_domain) {
    ScratchWorkspace ws("library-lookup-travel");
    ScratchState state("library-lookup-travel");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);
    ASSERT(f.tools.find("library_lookup") != nullptr);

    ASSERT_TRUE(runtime.set_state("coding", false));
    ASSERT(f.tools.find("library_lookup") == nullptr);
}

// ---------------------------------------------------------------------------
// Selection precision: a project's tags must pull what applies, and nothing else.
// ---------------------------------------------------------------------------

namespace {

// The selection line the scanner proposed, as the index writes it.
std::string selection_line_from(const std::string& proposal) {
    const std::size_t at = proposal.find("## Library selection");
    if (at == std::string::npos)
        return {};
    const std::size_t start = proposal.find('\n', at);
    if (start == std::string::npos)
        return {};
    const std::size_t end = proposal.find('\n', start + 1);
    return proposal.substr(start + 1,
                           end == std::string::npos ? std::string::npos : end - start - 1);
}

} // namespace

TEST(library_selection_pulls_only_what_a_rust_project_needs) {
    ScratchWorkspace ws("select-rust");
    ScratchState state("select-rust");
    write_text(ws.root() / "Cargo.toml", "[package]\nname = \"cli\"\nversion = \"0.1.0\"\n");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);

    const ToolResult proposed = call_tool(f, "index_init", json::object());
    ASSERT(proposed.ok);
    ASSERT(selection_line_from(proposed.output).find("#rust") != std::string::npos);

    // Confirmed, as the agent would after the user agreed.
    ASSERT(call_tool(f, "index_init", json{{"commit", proposed.output}}).ok);
    const std::string head = head_blocks_text(runtime);
    // The entry tagged for Rust applies...
    ASSERT(head.find("say who owns it") != std::string::npos);
    // ...and the C++ toolchain does not.
    ASSERT(head.find("still holds an iterator") == std::string::npos);
    ASSERT(head.find("behaves like a scripting language") == std::string::npos);
}

TEST(library_selection_for_a_python_project_does_not_leak_cpp) {
    ScratchWorkspace ws("select-python");
    ScratchState state("select-python");
    write_text(ws.root() / "requirements.txt", "django\npytest\n");
    write_text(ws.root() / "docker-compose.yml", "services:\n  db:\n");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);

    const ToolResult proposed = call_tool(f, "index_init", json::object());
    ASSERT(proposed.ok);
    // The scanner proposes only vocabulary tags, which is what makes them
    // selectable at all.
    ASSERT(proposed.output.find("#python") != std::string::npos);
    ASSERT(proposed.output.find("#django") != std::string::npos);
    ASSERT(proposed.output.find("#dockercompose") != std::string::npos);

    ASSERT(call_tool(f, "index_init", json{{"commit", proposed.output}}).ok);
    const std::string head = head_blocks_text(runtime);
    // Nothing from the C++ set applies here. (Nothing applies yet: python,
    // django and dockercompose have no entries -- tools/library_check.py reports
    // exactly that silence.)
    ASSERT(head.find("still holds an iterator") == std::string::npos);
    ASSERT(head.find("say who owns it") == std::string::npos);
}

TEST(profile_cpp_cmake_and_compose_selects_the_cpp_set) {
    ScratchWorkspace ws("profile-cpp");
    ScratchState state("profile-cpp");
    write_text(ws.root() / "CMakeLists.txt",
               "project(app CXX)\nadd_executable(app src/main.cpp)\n");
    write_text(ws.root() / "src" / "main.cpp", "int main() { return 0; }\n");
    write_text(ws.root() / "docker-compose.yml", "services:\n  app:\n");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);

    const ToolResult proposed = call_tool(f, "index_init", json::object());
    ASSERT(proposed.ok);
    for (const char* tag : {"#cpp", "#cmake", "#dockercompose"})
        ASSERT(proposed.output.find(tag) != std::string::npos);

    ASSERT(call_tool(f, "index_init", json{{"commit", proposed.output}}).ok);
    const std::string head = head_blocks_text(runtime);
    ASSERT(head.find("still holds an iterator") != std::string::npos);
    ASSERT(head.find("say who owns it") != std::string::npos);
    ASSERT(head.find("the PHP test runner") == std::string::npos);
}

TEST(profile_dotnet_angular_monorepo_reports_what_it_cannot_serve) {
    ScratchWorkspace ws("profile-dotnet");
    ScratchState state("profile-dotnet");
    write_text(ws.root() / "api.csproj",
               "<Project><PropertyGroup><TargetFramework>net8.0</TargetFramework></PropertyGroup>"
               "<ItemGroup><PackageReference Include=\"Microsoft.EntityFrameworkCore\" />"
               "</ItemGroup></Project>");
    write_text(ws.root() / "package.json", R"({"dependencies":{"@angular/core":"17"}})");
    write_text(ws.root() / "tsconfig.json", "{}\n");
    write_text(ws.root() / "docker-compose.yml", "services:\n  api:\n  web:\n");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);

    const ToolResult proposed = call_tool(f, "index_init", json::object());
    ASSERT(proposed.ok);
    for (const char* tag : {"#dotnet", "#efcore", "#angular", "#typescript", "#dockercompose"})
        ASSERT(proposed.output.find(tag) != std::string::npos);

    ASSERT(call_tool(f, "index_init", json{{"commit", proposed.output}}).ok);
    const std::string head = head_blocks_text(runtime);
    // The direction nobody writes: none of the C++ set applies here.
    ASSERT(head.find("still holds an iterator") == std::string::npos);
    ASSERT(head.find("say who owns it") == std::string::npos);
    ASSERT(head.find("which layer to reach for first") == std::string::npos);
}

TEST(profile_go_service_selects_nothing_yet) {
    ScratchWorkspace ws("profile-go");
    ScratchState state("profile-go");
    write_text(ws.root() / "go.mod", "module example.com/svc\n\ngo 1.22\n");
    write_text(ws.root() / "main.go", "package main\n\nfunc main() {}\n");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start(false);

    const ToolResult proposed = call_tool(f, "index_init", json::object());
    ASSERT(proposed.ok);
    ASSERT(proposed.output.find("#go") != std::string::npos);

    ASSERT(call_tool(f, "index_init", json{{"commit", proposed.output}}).ok);
    const std::string head = head_blocks_text(runtime);
    ASSERT(head.find("still holds an iterator") == std::string::npos);
    ASSERT(head.find("say who owns it") == std::string::npos);
}

TEST(mcp_plugin_verbs_answer_usage_without_a_server) {
    LiveMcp live("mcp-usage");
    const std::vector<std::pair<std::string, std::string>> verbs = {
        {"mcp", "show"},    {"mcp", "connect"}, {"mcp", "disconnect"},
        {"mcp", "refresh"}, {"mcp", "prompts"}, {"mcp", "enable"},
        {"mcp", "disable"}, {"mcp", "trust"},   {"prompt", ""},
    };
    for (const auto& [root, verb] : verbs)
        ASSERT_TRUE(run_verb(live.runtime, root, verb, "").find("usage:") != std::string::npos);
    // The root with an unexpected argument explains itself too.
    ASSERT_TRUE(run_verb(live.runtime, "mcp", "", "bogus").find("usage:") != std::string::npos);
    // /mcp and /mcp list both list the configured servers.
    ASSERT_TRUE(run_verb(live.runtime, "mcp", "", "").find("echo") != std::string::npos);
    ASSERT_TRUE(run_verb(live.runtime, "mcp", "list", "").find("echo") != std::string::npos);
}

TEST(mcp_plugin_verbs_report_unknown_servers) {
    LiveMcp live("mcp-unknown");
    ASSERT_TRUE(run_verb(live.runtime, "mcp", "show", "nope").find("nope") != std::string::npos);
    ASSERT_TRUE(run_verb(live.runtime, "mcp", "connect", "nope").find("nope") != std::string::npos);
    ASSERT_TRUE(run_verb(live.runtime, "mcp", "refresh", "nope").find("nope") != std::string::npos);
    ASSERT_TRUE(run_verb(live.runtime, "mcp", "enable", "nope").find("nope") != std::string::npos);
    ASSERT_TRUE(run_verb(live.runtime, "mcp", "disable", "nope").find("nope") != std::string::npos);
    // A trust toggle without on|off is a usage line, not a silent no-op.
    ASSERT_TRUE(run_verb(live.runtime, "mcp", "trust", "echo").find("usage:") != std::string::npos);
    ASSERT_TRUE(run_verb(live.runtime, "mcp", "prompts", "nope").find("not connected") !=
                std::string::npos);
    ASSERT_TRUE(run_verb(live.runtime, "prompt", "", "nope greet").find("not connected") !=
                std::string::npos);
}

TEST(mcp_plugin_verbs_work_against_a_live_server) {
    FillUi ui;
    LiveMcp live("mcp-live", &ui);
    // The auto-connect registered the fixture's tool through the plugin's port.
    ASSERT_TRUE((bool)live.f.tools.find("mcp_echo_echo_tool"));

    ASSERT_TRUE(run_verb(live.runtime, "mcp", "show", "echo").find("echo") != std::string::npos);
    ASSERT_TRUE(run_verb(live.runtime, "mcp", "prompts", "echo").find("greet") !=
                std::string::npos);

    // /prompt asks the server for the template and fills the input line.
    const std::string filled = run_verb(live.runtime, "prompt", "", "echo greet name=world");
    ASSERT_TRUE(filled.find("filled /prompt echo greet") != std::string::npos);
    ASSERT_TRUE(ui.filled.find("greet world") != std::string::npos);

    // /get mcp summarizes the servers and the prompts they expose.
    const std::string got = run_verb(live.runtime, "get", "mcp", "");
    ASSERT_TRUE(got.find("echo") != std::string::npos);
    ASSERT_TRUE(got.find("greet") != std::string::npos);

    // Disconnect unregisters the server's tools; connect brings them back.
    run_verb(live.runtime, "mcp", "disconnect", "echo");
    ASSERT_FALSE((bool)live.f.tools.find("mcp_echo_echo_tool"));
    ASSERT_TRUE(run_verb(live.runtime, "mcp", "connect", "echo").find("connected") !=
                std::string::npos);
    ASSERT_TRUE((bool)live.f.tools.find("mcp_echo_echo_tool"));

    // Refresh needs the connection; it reconnects and re-registers.
    ASSERT_TRUE(run_verb(live.runtime, "mcp", "refresh", "echo").find("refreshed") !=
                std::string::npos);
    ASSERT_TRUE((bool)live.f.tools.find("mcp_echo_echo_tool"));

    // Trust is a persisted toggle.
    ASSERT_TRUE(run_verb(live.runtime, "mcp", "trust", "echo on").find("trust: on") !=
                std::string::npos);
    ASSERT_TRUE(run_verb(live.runtime, "mcp", "trust", "echo off").find("trust: off") !=
                std::string::npos);

    // Disabling unregisters; re-enabling and refreshing work again.
    ASSERT_TRUE(run_verb(live.runtime, "mcp", "disable", "echo").find("disabled") !=
                std::string::npos);
    ASSERT_FALSE((bool)live.f.tools.find("mcp_echo_echo_tool"));
    ASSERT_TRUE(run_verb(live.runtime, "mcp", "enable", "echo").find("enabled") !=
                std::string::npos);
    // Disabling disconnected the server (correctly): refresh says so rather
    // than pretending, and connect brings it back.
    ASSERT_TRUE(run_verb(live.runtime, "mcp", "refresh", "echo").find("not connected") !=
                std::string::npos);
    ASSERT_TRUE(run_verb(live.runtime, "mcp", "connect", "echo").find("connected") !=
                std::string::npos);
}

// The read_resource tool the plugin contributes reads through the manager.
TEST(mcp_plugin_read_resource_reads_through_the_manager) {
    LiveMcp live("mcp-resource");
    auto tool = live.f.tools.find("read_resource");
    ASSERT_TRUE(tool != nullptr);
    auto ok = tool->execute({{"server", "echo"}, {"uri", "doc://greet"}});
    ASSERT_TRUE(ok.ok);
    ASSERT_TRUE(ok.output.find("hello doc://greet") != std::string::npos);
}

// A server that cannot be spawned shows as a failure on the bar; a runtime with
// no servers leaves the segment silent.
TEST(mcp_plugin_status_segment_reports_failures) {
    {
        ScratchWorkspace ws("mcp-segment-bad");
        ScratchConfig scratch("mcp-segment-bad");
        char buf[4096];
        const std::string cwd = getcwd(buf, sizeof buf) ? buf : ".";
        write_mcp_server_conf(scratch.root(), "bad", "no-such-mcp-binary", cwd);
        Fixture f;
        PluginRuntime runtime(f.tools, f.cfg, f.ws);
        runtime.add_bundled();
        runtime.start(false);
        std::string text = "<no mcp segment>";
        for (const auto& s : runtime.status().render(StatusSnapshot{}))
            if (s.id == "mcp")
                text = s.text;
        ASSERT_TRUE(text.find("!bad") != std::string::npos);
    }
    {
        ScratchWorkspace ws("mcp-segment-empty");
        ScratchConfig scratch("mcp-segment-empty");
        Fixture f;
        PluginRuntime runtime(f.tools, f.cfg, f.ws);
        runtime.add_bundled();
        runtime.start(false);
        for (const auto& s : runtime.status().render(StatusSnapshot{}))
            if (s.id == "mcp")
                ASSERT_TRUE(s.text.empty());
    }
}

// With no manager the whole host-facing API fails closed and says so, the way
// the CLI and a disabled plugin's stale command line find it.
TEST(mcp_plugin_inactive_api_says_not_active) {
    plugins::McpPlugin plugin;
    ASSERT_TRUE(plugin.manager() == nullptr);
    ASSERT_TRUE(plugin.connect("x").find("not active") != std::string::npos);
    ASSERT_TRUE(plugin.refresh("x").find("not active") != std::string::npos);
    ASSERT_TRUE(plugin.set_enabled("x", false).find("not active") != std::string::npos);
    ASSERT_TRUE(plugin.set_trusted("x", false).find("not active") != std::string::npos);
    ASSERT_TRUE(plugin.list_lines().empty());
    std::string error;
    ASSERT_TRUE(plugin.show_lines("x", error).empty());
    ASSERT_TRUE(error.find("not active") != std::string::npos);
    std::string text;
    ASSERT_TRUE(plugin.prompt_text("x", "y", json::object(), text).find("not active") !=
                std::string::npos);
    plugin.disconnect("x"); // a no-op, not a crash
    ASSERT_EQ(plugin.capabilities().size(), 7u);
    ASSERT_EQ(plugin.category(), "integration");
    plugin.shutdown();
}

// The skill tools are ordinary plugin contributions: one plugin, three tools,
// and the plugin's own prose. Nothing registers them from the Agent any more.
TEST(runtime_bundles_the_skill_tools) {
    ScratchConfig scratch("skill-tools");
    Fixture f;
    JobService jobs;
    TodoStore todos;
    SubAgentExecutor subagents;
    HostServices host{&jobs, &todos, &subagents, &f.cfg.cancel_token};

    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.attach_host_services(host);
    runtime.add_bundled();
    runtime.start();

    ASSERT_TRUE(runtime.status("tool_skills").enabled);
    ASSERT_TRUE((bool)f.tools.find("read_skill"));
    ASSERT_TRUE((bool)f.tools.find("list_skills"));
    ASSERT_TRUE((bool)f.tools.find("write_skill"));
}

TEST(disabling_tool_skills_removes_its_tools_and_prose) {
    ScratchConfig scratch("skill-tools-off");
    Fixture f;
    JobService jobs;
    TodoStore todos;
    SubAgentExecutor subagents;
    HostServices host{&jobs, &todos, &subagents, &f.cfg.cancel_token};

    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.attach_host_services(host);
    runtime.add_bundled();
    runtime.start();

    ASSERT_TRUE((bool)f.tools.find("read_skill"));
    ASSERT_TRUE(system_blocks_text(runtime).find("Skills are reusable procedures") !=
                std::string::npos);

    // The tools and the prose describing them leave together: a prompt that
    // documents a tool the registry cannot serve is the staleness §3.4 exists
    // to prevent.
    ASSERT_TRUE(runtime.set_state("tool_skills", false));
    ASSERT_FALSE((bool)f.tools.find("read_skill"));
    ASSERT_FALSE((bool)f.tools.find("list_skills"));
    ASSERT_FALSE((bool)f.tools.find("write_skill"));
    ASSERT_TRUE(system_blocks_text(runtime).find("Skills are reusable procedures") ==
                std::string::npos);
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

    // Take away the file reader: the read_skill/list_skills pair still carries
    // the Read role, so nothing is lost yet.
    ASSERT_TRUE(runtime.set_state("tool_read", false));
    ASSERT_TRUE(runtime.audit().empty());

    // Take away the skill tools too, and no enabled tool can read at all - the
    // audit says so.
    ASSERT_TRUE(runtime.set_state("tool_skills", false));
    auto findings = runtime.audit();
    ASSERT_EQ(findings.size(), 1u);
    ASSERT_TRUE(findings[0].kind == AuditFinding::Kind::Deficiency);
    ASSERT(findings[0].message.find("read") != std::string::npos);

    // Restoring one reader clears the warning - on demand means never stale.
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
    ASSERT_EQ(*view.snapshot.credits_balance, 42.5);
}

TEST(wallet_registry_installs_and_unwinds) {
    ToolRegistry tools;
    PromptRegistry prompts;
    StatusRegistry status;
    PanelRegistry panels;
    WalletRegistry wallets;
    EventBus bus;
    CommandRegistry commands;
    SettingsRegistry settings;
    auto search_backends = std::make_shared<SearchBackendRegistry>();
    PluginServices services(tools, prompts, status, panels, wallets, search_backends, bus,
                            commands);
    services.set_settings(settings);
    services.set_owner("acme");

    WalletCapability cap([](const Config&) -> std::optional<WalletSnapshot> {
        return WalletSnapshot::of_balance(7.0);
    });
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
    Event raw{&ended, false};
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
    ASSERT_EQ(*runtime.wallet().snapshot.credits_balance, 10.0);

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
    ASSERT_EQ(*fresh.snapshot.credits_balance, 20.0);
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
// A provider that meters quota windows and reports no balance: the other shape
// of the same capability.
// ---------------------------------------------------------------------------

class QuotaProbePlugin : public IPlugin {
public:
    static WalletSnapshot fixed_snapshot;

    std::string id() const override { return "quotaprobe"; }
    std::string version() const override { return "1.0.0"; }
    std::string name() const override { return "Quota probe"; }

    bool initialize(const PluginContext&) override { return true; }
    void shutdown() override {}

    std::vector<std::unique_ptr<Capability>> capabilities() override {
        std::vector<std::unique_ptr<Capability>> caps;
        caps.push_back(std::make_unique<WalletCapability>(
            [](const Config&) -> std::optional<WalletSnapshot> { return fixed_snapshot; }));
        return caps;
    }
};

WalletSnapshot QuotaProbePlugin::fixed_snapshot;

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

TEST(wallet_segment_shows_closest_window) {
    ScratchConfig scratch("wallet_closest");
    Fixture f;
    QuotaProbePlugin::fixed_snapshot = WalletSnapshot{};
    QuotaProbePlugin::fixed_snapshot.plan = "Go";
    QuotaProbePlugin::fixed_snapshot.unit = "percent";
    WalletWindow w5h;
    w5h.label = "5h";
    w5h.percent_used = 80.0;
    WalletWindow w7d;
    w7d.label = "7d";
    w7d.percent_used = 30.0;
    WalletWindow wM;
    wM.label = "monthly";
    wM.percent_used = 20.0;
    QuotaProbePlugin::fixed_snapshot.windows = {w5h, w7d, wM};

    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add(std::make_shared<QuotaProbePlugin>());
    runtime.start();
    Config live;
    live.provider_name = "quotaprobe";
    live.api_key = "key";
    runtime.attach_config(live);
    runtime.perform_wallet_refresh();

    const auto find_wallet = [&runtime]() -> std::optional<StatusSegment> {
        for (const auto& s : runtime.status().render(StatusSnapshot{}))
            if (s.id == "wallet")
                return s;
        return std::nullopt;
    };
    auto seg = find_wallet();
    ASSERT(seg.has_value());
    ASSERT(seg->text.find("80%") != std::string::npos);
    ASSERT(seg->text.find("5h") != std::string::npos);
    ASSERT(seg->tone == StatusTone::Warn);
}

TEST(wallet_segment_breaks_ties_by_shortest_label) {
    ScratchConfig scratch("wallet_tie");
    Fixture f;
    QuotaProbePlugin::fixed_snapshot = WalletSnapshot{};
    WalletWindow w5h;
    w5h.label = "5h";
    w5h.percent_used = 50.0;
    WalletWindow w7d;
    w7d.label = "7d";
    w7d.percent_used = 50.0;
    QuotaProbePlugin::fixed_snapshot.windows = {w7d, w5h};

    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add(std::make_shared<QuotaProbePlugin>());
    runtime.start();
    Config live;
    live.provider_name = "quotaprobe";
    live.api_key = "key";
    runtime.attach_config(live);
    runtime.perform_wallet_refresh();

    const auto find_wallet = [&runtime]() -> std::optional<StatusSegment> {
        for (const auto& s : runtime.status().render(StatusSnapshot{}))
            if (s.id == "wallet")
                return s;
        return std::nullopt;
    };
    auto seg = find_wallet();
    ASSERT(seg.has_value());
    ASSERT(seg->text.find("5h") != std::string::npos);
}

TEST(wallet_segment_hides_when_disabled) {
    ScratchConfig scratch("wallet_hide");
    Fixture f;
    QuotaProbePlugin::fixed_snapshot = WalletSnapshot{};
    WalletWindow w;
    w.label = "5h";
    w.percent_used = 50.0;
    QuotaProbePlugin::fixed_snapshot.windows = {w};

    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add(std::make_shared<QuotaProbePlugin>());
    runtime.start();
    Config live;
    live.provider_name = "quotaprobe";
    live.api_key = "key";
    live.wallet_enabled = false;
    runtime.attach_config(live);
    runtime.perform_wallet_refresh();

    const auto find_wallet = [&runtime]() -> std::optional<StatusSegment> {
        for (const auto& s : runtime.status().render(StatusSnapshot{}))
            if (s.id == "wallet")
                return s;
        return std::nullopt;
    };
    ASSERT_FALSE(find_wallet().has_value());
}

// The rule for a provider that reports both, in one place: the balance answers
// "what is left?" most directly, so it wins on the bar; the windows are still
// there for `/get provider wallet`.
TEST(wallet_segment_prefers_the_balance_over_windows) {
    ScratchConfig scratch("wallet_balance_wins");
    Fixture f;
    QuotaProbePlugin::fixed_snapshot = WalletSnapshot{};
    WalletWindow w;
    w.label = "5h";
    w.percent_used = 90.0;
    QuotaProbePlugin::fixed_snapshot.windows = {w};
    QuotaProbePlugin::fixed_snapshot.credits_balance = 12.5;

    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add(std::make_shared<QuotaProbePlugin>());
    runtime.start();
    Config live;
    live.provider_name = "quotaprobe";
    live.api_key = "key";
    runtime.attach_config(live);
    runtime.perform_wallet_refresh();

    for (const auto& s : runtime.status().render(StatusSnapshot{})) {
        if (s.id != "wallet")
            continue;
        ASSERT(s.text.find("12.50") != std::string::npos);
        ASSERT(s.text.find("90%") == std::string::npos);
        return;
    }
    ASSERT(false);
}

TEST(wallet_segment_shows_dash_for_unsupported_provider) {
    ScratchConfig scratch("wallet_dash");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.start();
    Config live;
    live.provider_name = "custom";
    runtime.attach_config(live);

    const auto find_wallet = [&runtime]() -> std::optional<StatusSegment> {
        for (const auto& s : runtime.status().render(StatusSnapshot{}))
            if (s.id == "wallet")
                return s;
        return std::nullopt;
    };
    auto seg = find_wallet();
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

namespace {

// Contributes one slash-command namespace, so activation has a command to
// install and disable has one to unwind.
class CommandPlugin : public IPlugin {
public:
    explicit CommandPlugin(std::string id = "cmd") : id_(std::move(id)) {}

    std::string id() const override { return id_; }
    std::string version() const override { return "1.0.0"; }
    std::string name() const override { return "Command probe"; }

    bool initialize(const PluginContext&) override { return true; }
    void shutdown() override {}

    std::vector<std::unique_ptr<Capability>> capabilities() override {
        CommandSpec spec;
        spec.name = "hello";
        spec.help = "Say hello";
        spec.man = "Usage: /hello greet <name>";
        spec.subtree = json::parse(R"({"greet":{"help":"Greet someone"}})");
        spec.handlers["greet"] = [](const std::string& arg) {
            return "Hello, " + (arg.empty() ? std::string("world") : arg) + "!";
        };
        std::vector<std::unique_ptr<Capability>> caps;
        caps.push_back(std::make_unique<CommandCapability>(std::move(spec)));
        return caps;
    }

private:
    std::string id_;
};

} // namespace

TEST(runtime_command_capability_installs_and_unwinds) {
    ScratchConfig scratch("command");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add(std::make_shared<CommandPlugin>());

    ASSERT_EQ(runtime.commands().size(), 0u);

    runtime.start();
    ASSERT_TRUE(runtime.status("cmd").enabled);
    ASSERT_EQ(runtime.commands().size(), 1u);

    const auto& all = runtime.commands().all();
    ASSERT_EQ(all[0].owner, std::string("cmd"));
    ASSERT_EQ(all[0].spec.name, std::string("hello"));
    ASSERT_EQ(all[0].spec.handlers.count("greet"), 1u);
    ASSERT_EQ(all[0].spec.handlers.at("greet")("amber"), std::string("Hello, amber!"));

    ASSERT_TRUE(runtime.set_state("cmd", false));
    ASSERT_EQ(runtime.commands().size(), 0u);
}

TEST(runtime_command_contribution_is_listed_and_removed) {
    ScratchConfig scratch("command-list");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add(std::make_shared<CommandPlugin>());
    runtime.start();

    bool listed = false;
    const auto enabled = runtime.status("cmd");
    for (const auto& c : enabled.contributions)
        if (c.kind == CapabilityKind::Command && c.name == "hello")
            listed = true;
    ASSERT_TRUE(listed);

    ASSERT_TRUE(runtime.set_state("cmd", false));
    const auto disabled = runtime.status("cmd");
    for (const auto& c : disabled.contributions)
        ASSERT_FALSE(c.kind == CapabilityKind::Command);
}

// A tool-contributing plugin survives a disable/re-enable cycle: install is
// replayed on activation, so the factory must run again.
TEST(runtime_tool_capability_returns_after_reenable) {
    ScratchConfig scratch("tool-reenable");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add(std::make_shared<NamedToolPlugin>("alpha", "alpha_tool"));
    runtime.start();

    ASSERT_TRUE((bool)f.tools.find("alpha_tool"));
    ASSERT_TRUE(runtime.set_state("alpha", false));
    ASSERT_FALSE((bool)f.tools.find("alpha_tool"));
    ASSERT_TRUE(runtime.set_state("alpha", true));
    ASSERT_TRUE((bool)f.tools.find("alpha_tool"));
}

TEST(bundled_hello_plugin_contributes_the_hello_command) {
    ScratchConfig scratch("hello-command");
    Fixture f;
    PluginRuntime runtime(f.tools, f.cfg, f.ws);
    runtime.add_bundled();
    runtime.start();

    ASSERT_TRUE(runtime.status("hello").enabled);
    // The bundled set contributes six namespaces: hello, the project index's
    // index, the mcp plugin's mcp / prompt / get, and watch.
    ASSERT_EQ(runtime.commands().size(), 6u);
    const auto& all = runtime.commands().all();
    const auto hello =
        std::find_if(all.begin(), all.end(),
                     [](const agent::CommandRegistry::Entry& e) { return e.spec.name == "hello"; });
    ASSERT(hello != all.end());
    ASSERT_EQ(hello->spec.handlers.at("greet")("amber"), std::string("Hello, amber!"));

    // A command-contributing plugin survives a disable/re-enable cycle: the
    // capability must come back with the same binding.
    ASSERT_TRUE(runtime.set_state("hello", false));
    ASSERT_FALSE(runtime.status("hello").enabled);
    ASSERT_EQ(runtime.commands().size(), 5u); // the mcp plugin's three, watch and the index survive
    ASSERT_TRUE(runtime.set_state("hello", true));
    ASSERT_TRUE(runtime.status("hello").enabled);
    ASSERT_EQ(runtime.commands().size(), 6u);
}

// The registry console groups contributions by kind, so every kind needs a
// name: an unnamed one renders as "?" and the contribution reads as a typo.
TEST(capability_kind_name_covers_every_kind) {
    const std::vector<CapabilityKind> kinds = {
        CapabilityKind::Tool,          CapabilityKind::ToolSource, CapabilityKind::PromptBlock,
        CapabilityKind::StatusSegment, CapabilityKind::Panel,      CapabilityKind::Provider,
        CapabilityKind::Wallet,        CapabilityKind::Command,    CapabilityKind::SearchBackend,
        CapabilityKind::Skill,
    };
    for (const auto kind : kinds) {
        const std::string name = capability_kind_name(kind);
        ASSERT_FALSE(name.empty());
        ASSERT(name != "?");
    }
    ASSERT_EQ(std::string(capability_kind_name(CapabilityKind::Command)), std::string("command"));
    ASSERT_EQ(std::string(capability_kind_name(CapabilityKind::SearchBackend)),
              std::string("search"));
}
