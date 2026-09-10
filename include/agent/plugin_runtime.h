#ifndef AGENT_PLUGIN_RUNTIME_H
#define AGENT_PLUGIN_RUNTIME_H

// The plugin runtime: one composition root for both hosts (spec §9).
//
// It owns the contribution registries, installs declared capabilities through
// the ledger, tracks which plugins are enabled, and persists that state in
// ~/.config/amber/plugins/<id>/plugin.conf. Disabling a plugin unwinds its
// ledger: nothing it contributed stays reachable.
//
// State has exactly one source of truth (that file), and one write path
// (`set_state`, backing `/set plugin <id> on|off`), so the command tree and
// the runtime cannot drift.

#include "agent/config.h"
#include "agent/event_bus.h"
#include "agent/extensions.h"
#include "agent/plugin.h"
#include "agent/plugin_capability.h"
#include "agent/plugin_registry.h"
#include "agent/registry.h"
#include "agent/workspace.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace agent {

class PluginRuntime {
public:
    PluginRuntime(ToolRegistry& tools, const Config& config, const Workspace& workspace);
    ~PluginRuntime();

    PluginRuntime(const PluginRuntime&) = delete;
    PluginRuntime& operator=(const PluginRuntime&) = delete;

    // --- Registration ------------------------------------------------------

    // Register a compiled-in plugin. Registration never activates: state comes
    // from the persisted file, so a restart reproduces the same harness.
    // Returns false when the id is already taken (first registration wins).
    bool add(std::shared_ptr<IPlugin> plugin, bool bundled = true);

    // Register every plugin that ships with amber. Bundled plugins are defined
    // in one place so the shipped set is enumerable at a glance.
    void add_bundled();

    // Register the discovered external (v1) plugins under the same surface, so
    // one command tree controls both tiers. A discovered plugin whose id
    // collides with a bundled one is skipped, never silently overwritten.
    void add_external(PluginManager& manager);

    // Point the runtime (and every plugin context it hands out) at the host's
    // LIVE config. A host that takes its Config by value must call this after
    // construction, so plugins read what the user has changed, not a snapshot
    // taken at startup.
    void attach_config(const Config& config);

    // --- Lifecycle ---------------------------------------------------------

    // Activate every plugin whose persisted state says it is on. Called once
    // by the host after registration. Idempotent.
    void start();

    // Stop every active plugin and unwind its contributions. Idempotent.
    void shutdown();

    // Forward the host's UI tick to active plugins (time-driven work). Runs on
    // the UI thread: a plugin must not block here.
    void tick();

    // --- State (backs /get plugin and /set plugin) -------------------------

    struct PluginStatus {
        std::string id;
        std::string version;
        std::string tier; // "bundled" or "external"
        bool enabled = false;
        std::vector<ExtensionItem> contributions;
    };

    std::vector<PluginStatus> list() const;
    bool has(const std::string& id) const;
    PluginStatus status(const std::string& id) const;

    // Non-owning access to a registered plugin (null when unknown). Hosts and
    // tests use it to reach a plugin's own state; the runtime keeps ownership.
    IPlugin* find(const std::string& id) const noexcept;

    // The single write path. Persists first, then applies, so a failed
    // activation does not leave a plugin recorded as on.
    bool set_state(const std::string& id, bool on);

    // --- Registries (hosts pull from these) --------------------------------

    PromptRegistry& prompts() noexcept { return prompts_; }
    CommandRegistry& commands() noexcept { return commands_; }
    StatusRegistry& status() noexcept { return status_; }
    PanelRegistry& panels() noexcept { return panels_; }
    const PanelRegistry& panels() const noexcept { return panels_; }
    PluginSettingsStore& settings() noexcept { return settings_; }
    EventBus& events() noexcept { return bus_; }

    // Contributions across every registry, for the console.
    std::vector<ExtensionItem> contributions() const;

private:
    // Install one plugin's declared capabilities, recording each in the ledger.
    bool install_capabilities(const std::string& id, IPlugin& plugin);
    bool activate(const std::string& id);
    void deactivate(const std::string& id);

    ToolRegistry* tools_;
    PromptRegistry prompts_;
    CommandRegistry commands_;
    StatusRegistry status_;
    PanelRegistry panels_;
    PluginSettingsStore settings_;
    EventBus bus_;
    PluginLedger ledger_;
    PluginRegistry registry_;
    std::unique_ptr<PluginServices> services_;
    std::unique_ptr<PluginContext> context_;
    Config config_; // fallback until the host attaches its own
    const Config* live_config_ = nullptr;
    const Workspace* workspace_;

    struct Entry {
        std::shared_ptr<IPlugin> plugin;
        bool bundled = true;
        // What the plugin declared at registration; installed on activation.
        std::vector<std::unique_ptr<Capability>> declared;
    };
    std::map<std::string, Entry> plugins_;
};

} // namespace agent

#endif // AGENT_PLUGIN_RUNTIME_H
