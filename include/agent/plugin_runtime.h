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
#include "agent/events.h"
#include "agent/extensions.h"
#include "agent/plugin.h"
#include "agent/plugin_capability.h"
#include "agent/plugin_registry.h"
#include "agent/registry.h"
#include "agent/toolset_audit.h"
#include "agent/workspace.h"

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace agent {

class PluginRuntime {
public:
    // `config` is taken by value: it is kept as the fallback configuration
    // until the host attaches its own, so this constructor owns a copy either
    // way.
    PluginRuntime(ToolRegistry& tools, Config config, const Workspace& workspace);
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

    // Point the runtime at the host's services (job service, todo store,
    // sub-agent executor, cancel token). Tools need them at construction, so
    // this is what lets the core's tools be plugin contributions. Attach before
    // start(), like the config.
    void attach_host_services(const HostServices& host) noexcept;

    // --- Lifecycle ---------------------------------------------------------

    // Activate every plugin whose persisted state says it is on. Called once
    // by the host after registration. Idempotent.
    //
    // `use_persisted_state` false starts from the declared defaults instead -
    // every bundled plugin on, nothing read from the user's configuration. A
    // harness measures the shipped configuration; one machine's saved
    // preferences are not part of it, and a result that silently depended on
    // them would not be reproducible anywhere else.
    void start(bool use_persisted_state = true);

    // Stop every active plugin and unwind its contributions. Idempotent.
    void shutdown();

    // Forward the host's UI tick to active plugins (time-driven work). Runs on
    // the UI thread: a plugin must not block here.
    void tick();

    // --- State (backs /get plugin and /set plugin) -------------------------

    struct PluginStatus {
        std::string id;
        std::string version;
        std::string tier;        // "bundled" or "external"
        std::string description; // one-line summary, may be empty
        std::string category;    // grouping for the registry list
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

    // Apply a state without persisting it. For a run that must not touch the
    // user's saved configuration: the benchmark harness switches a plugin off
    // to measure the difference, and that experiment is not a preference.
    // Installation and unwinding go through the same ledger either way - only
    // the persistence differs.
    bool apply_state(const std::string& id, bool on);

    // --- Wallet (the active provider's balance) ----------------------------

    // What the status bar and `/get provider wallet` report. `supported` is
    // whether the active provider declared a wallet at all; the rest describes
    // the last refresh.
    struct WalletView {
        bool enabled = true; // the display switch (a user preference)
        bool supported = false;
        bool ready = false;  // a fetch produced a value
        bool failed = false; // the last fetch did not
        double amount = 0.0;
        std::string holder; // provider id, for the command output
    };

    // State shared with an in-flight fetch.
    //
    // The fetch runs on a worker, so everything it touches must outlive this
    // runtime and must not be read from the host's live state: the host mutates
    // its Config (provider switches, /set model) while a fetch is in flight.
    // The worker therefore writes **only the atomics here**, and whatever it
    // needs is copied on the host thread before the thread starts. `provider`
    // and the ticket fields are host-thread-only bookkeeping, which is what
    // keeps a fetch that lands after a provider switch from being shown under
    // the new provider.
    struct WalletState {
        std::atomic<bool> inflight{false};
        std::atomic<bool> has_value{false}; // the ticket answered with an amount
        std::atomic<bool> failed{false};
        std::atomic<double> amount{0.0};
        std::atomic<long long> active_ticket{0};
        std::atomic<long long> result_ticket{0};
        std::atomic<long long> last_ms{0};
        std::string provider; // the provider the active ticket fetches for
    };

    WalletView wallet() const;
    bool wallet_enabled() const;

    // Mark the wallet stale so the next tick refreshes it: called when a turn
    // ends and when the active provider changes.
    void request_wallet_refresh() noexcept;

    // One synchronous refresh of the active provider's wallet. Public so tests
    // can drive it directly; never call it on a thread that must stay
    // responsive (it performs the plugin's I/O).
    void perform_wallet_refresh();

    // --- Allowance (the active provider's subscription/quota windows) ------

    struct AllowanceView {
        bool enabled = true;
        bool supported = false;
        bool ready = false;
        bool failed = false;
        AllowanceSnapshot snapshot;
        std::string holder;
    };

    // State shared with an in-flight allowance fetch, same pattern as
    // WalletState: the worker writes only the atomics, the snapshot is copied
    // on the host thread before the fetch starts.
    struct AllowanceState {
        std::atomic<bool> inflight{false};
        std::atomic<bool> has_value{false};
        std::atomic<bool> failed{false};
        std::atomic<long long> active_ticket{0};
        std::atomic<long long> result_ticket{0};
        std::atomic<long long> last_ms{0};
        std::string provider;
        AllowanceSnapshot snapshot; // written by the worker under mutex
        mutable std::mutex mutex;
    };

    AllowanceView allowance() const;
    bool allowance_enabled() const;
    void request_allowance_refresh() noexcept;
    void perform_allowance_refresh();

    // --- Registries (hosts pull from these) --------------------------------

    PromptRegistry& prompts() noexcept { return prompts_; }
    StatusRegistry& status() noexcept { return status_; }
    PanelRegistry& panels() noexcept { return panels_; }
    const PanelRegistry& panels() const noexcept { return panels_; }
    WalletRegistry& wallets() noexcept { return wallets_; }
    const WalletRegistry& wallets() const noexcept { return wallets_; }
    AllowanceRegistry& allowances() noexcept { return allowances_; }
    const AllowanceRegistry& allowances() const noexcept { return allowances_; }
    EventBus& events() noexcept { return bus_; }

    // Contributions across every registry, for the console.
    std::vector<ExtensionItem> contributions() const;

    // Audit the enabled toolset against the current plugin state. Computed on
    // demand, so it can never describe a configuration that has moved on; the
    // caller shows the findings after a change (spec §5.2). Empty means the
    // set is complete enough and its tools are distinguishable.
    std::vector<AuditFinding> audit() const;

private:
    // Install one plugin's declared capabilities, recording each in the ledger.
    bool install_capabilities(const std::string& id, IPlugin& plugin);
    bool activate(const std::string& id);
    void deactivate(const std::string& id);

    // Schedule a wallet refresh when one is due: stale, not already running,
    // and past the floor since the last one.
    void maybe_refresh_wallet();
    // Snapshot everything a fetch needs and start (or answer) the current
    // ticket. Host thread only.
    void schedule_wallet_fetch();
    // Invoke the fetch and record the result against its ticket. Reads only its
    // arguments and the shared atomics, so it is safe on a detached worker that
    // outlives this runtime.
    static void run_wallet_fetch(const std::shared_ptr<WalletState>& state, long long ticket,
                                 const WalletRegistry::Fetch& fetch, const Config& cfg);
    // Allowance refresh helpers, same structure as the wallet ones. The readout
    // itself is declared by install_core_ui, with the rest of the core UI.
    void maybe_refresh_allowance();
    void schedule_allowance_fetch();
    static void run_allowance_fetch(const std::shared_ptr<AllowanceState>& state, long long ticket,
                                    const AllowanceRegistry::Fetch& fetch, const Config& cfg);

    // Install the harness's own UI - the status-bar segments, the wallet
    // readout and the registry console - through the same declare-and-install
    // path a plugin's capabilities take, under a reserved "core" owner. Core
    // and plugin contributions therefore cannot drift apart, and an install
    // that is broken cannot hide behind "no plugin uses that capability yet".
    // Not ledgered: core UI lives as long as the runtime and is never disabled.
    void install_core_ui();
    const Config& active_config() const noexcept { return live_config_ ? *live_config_ : config_; }

    PromptRegistry prompts_;
    StatusRegistry status_;
    PanelRegistry panels_;
    WalletRegistry wallets_;
    EventBus bus_;
    Subscription wallet_turn_sub_;
    std::atomic<bool> wallet_dirty_{false};
    // Shared with an in-flight fetch, which owns a reference so the state
    // outlives this runtime if a fetch is still running when it is destroyed.
    // The worker touches only the atomics inside; `provider` and the ticket
    // counter below are host-thread-only.
    std::shared_ptr<WalletState> wallet_state_ = std::make_shared<WalletState>();
    long long wallet_ticket_ = 0; // host thread only
    AllowanceRegistry allowances_;
    Subscription allowance_turn_sub_;
    std::atomic<bool> allowance_dirty_{false};
    std::shared_ptr<AllowanceState> allowance_state_ = std::make_shared<AllowanceState>();
    long long allowance_ticket_ = 0;
    PluginLedger ledger_;
    PluginRegistry registry_;
    std::unique_ptr<PluginServices> services_;
    std::unique_ptr<PluginContext> context_;
    Config config_; // fallback until the host attaches its own
    const Config* live_config_ = nullptr;
    HostServices host_services_; // pointers set by the host, which owns them
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
