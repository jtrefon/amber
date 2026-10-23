#ifndef AMBER_PLUGINS_WATCH_PLUGIN_H
#define AMBER_PLUGINS_WATCH_PLUGIN_H

#include "watch_observer.h"
#include "watch_runner.h"
#include "watch_service.h"

#include "agent/plugin_core.h"

#include <map>
#include <memory>
#include <string>

namespace agent::plugins {

// A standalone observability plugin: background watches, a deterministic
// journal, PR check polling, and a one-shot read-only observer. It owns no
// core state; the host routes its reports to the owning window.
class WatchPlugin final : public agent::IPlugin {
public:
    WatchPlugin() = default;
    // Test seam: inject a fake runner/observer and an isolated state dir.
    WatchPlugin(std::shared_ptr<CommandRunner> runner, std::shared_ptr<ObserverModel> observer,
                std::string state_dir);

    std::string id() const override { return "watch"; }
    std::string version() const override { return "1.0.0"; }
    std::string name() const override { return "Watch"; }
    std::string description() const override {
        return "Background watches with a small read-only observer (PR sentry).";
    }
    std::string category() const override { return agent::plugin_category::kObservability; }

    bool initialize(const agent::PluginContext& ctx) override;
    void shutdown() override;
    // Cancel every live watch owned by a window/session (window close).
    void cancel_owner(const std::string& owner_id);
    // The owner is gone: terminal reports must not be re-emitted to it.
    void abandon_owner(const std::string& owner_id);
    // Host acknowledgement, persisted so restart cannot repeat a delivered turn.
    void ack(const std::string& watch_id);
    // Called by the host after its windows are restored.
    void reemit_pending();
    std::vector<WatchReport> pending_reports() const;
    std::vector<WatchRecord> list_records() const;
    std::string show_text(const std::string& id) const;
    // Observer usage per owner window, drained for the host's usage ledger.
    std::map<std::string, agent::UsageTotals> take_usage();
    bool stop_watch(const std::string& id);
    int running_count() const;
    std::vector<std::unique_ptr<agent::Capability>> capabilities() override;

private:
    WatchService* ensure_service(agent::PluginServices& services);
    std::string policy_block() const;
    std::string command_text(const std::string& verb, const std::string& arg) const;
    std::unique_ptr<agent::Capability> make_tool_capability();
    std::unique_ptr<agent::Capability> make_policy_capability();
    std::unique_ptr<agent::Capability> make_status_capability();
    std::unique_ptr<agent::Capability> make_panel_capability();
    std::unique_ptr<agent::Capability> make_command_capability() const;
    std::unique_ptr<agent::Capability> make_settings_capability();

    const agent::ToolRegistry* tools_ = nullptr;
    std::shared_ptr<CommandRunner> injected_runner_;
    std::shared_ptr<ObserverModel> injected_observer_;
    std::string state_dir_;
    WatchSettings settings_;
    std::unique_ptr<WatchService> service_;
};

} // namespace agent::plugins

#endif // AMBER_PLUGINS_WATCH_PLUGIN_H
