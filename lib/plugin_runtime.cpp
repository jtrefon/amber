#include "agent/plugin_runtime.h"

#include "agent/core_segments.h"
#include "agent/dialect.h"
#include "agent/plugin_console.h"
#include "agent/plugins_bundled.h"
#include "agent/plugin_v1_adapter.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace agent {

namespace {

namespace fs = std::filesystem;

// Core contributions are recorded under this reserved owner. "core" is not a
// registered plugin, so `/set plugin core off` cannot unwind the harness's own
// UI; the owner exists so core goes through the same declare-and-install path a
// plugin does — the path a broken install would otherwise hide in.
constexpr const char* kCoreOwner = "core";

// Plugin state lives beside a user-installed plugin's manifest, under the same
// config tree the provider files already use.
std::string plugin_dir(const std::string& id) {
    return global_config_dir() + "/plugins/" + id;
}

std::string state_path(const std::string& id) {
    return plugin_dir(id) + "/plugin.conf";
}

// A missing file means "never configured" -> the default applies (bundled
// plugins ship on).
bool read_enabled(const std::string& id, bool default_value) {
    std::ifstream f(state_path(id));
    if (!f)
        return default_value;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("enabled=", 0) != 0)
            continue;
        const std::string value = line.substr(8);
        return value == "1" || value == "true" || value == "on";
    }
    return default_value;
}

bool write_enabled(const std::string& id, bool enabled) {
    std::error_code ec;
    fs::create_directories(plugin_dir(id), ec);
    std::ofstream f(state_path(id), std::ios::trunc);
    if (!f)
        return false;
    f << "# amber plugin state: " << id << "\n";
    f << "enabled=" << (enabled ? 1 : 0) << "\n";
    return static_cast<bool>(f);
}

} // namespace

PluginRuntime::PluginRuntime(ToolRegistry& tools, Config config, const Workspace& workspace)
    : config_(std::move(config)), workspace_(&workspace) {
    services_ = std::make_unique<PluginServices>(tools, prompts_, status_, panels_, wallets_,
                                                 bus_);
    context_ = std::make_unique<PluginContext>(PluginContext{bus_, tools, &config_, *workspace_});
    services_->config = &config_;
    registry_.set_context(context_.get());

    // The wallet belongs to the active provider and is refreshed when a turn
    // ends rather than on a timer: a balance only moves because we spent
    // something.
    wallet_turn_sub_ = Events(bus_).subscribe<TurnEndedEvent>(
        [this](const TurnEndedEvent&) { request_wallet_refresh(); });

    // Every readout is core, so every provider renders through one path: a
    // plugin supplies a fetch, never a poll loop, a cache and a segment.
    install_core_ui();
}

PluginRuntime::~PluginRuntime() {
    shutdown();
}

bool PluginRuntime::add(std::shared_ptr<IPlugin> plugin, bool bundled) {
    if (!plugin)
        return false;
    const std::string id = plugin->id();
    if (plugins_.find(id) != plugins_.end())
        return false; // first registration wins
    Entry entry;
    entry.plugin = std::move(plugin);
    entry.bundled = bundled;
    // Declarations are a property of the plugin, not of its activation: a
    // plugin that ships switched off still declares the protocol it would
    // provide, so a provider file pointing at that flavor reports "the plugin
    // is disabled" rather than silently speaking another wire protocol.
    entry.declared = entry.plugin->capabilities();
    for (const auto& capability : entry.declared) {
        if (capability && capability->kind() == CapabilityKind::Provider)
            declare_flavor(capability->name(), id);
    }
    plugins_[id] = std::move(entry);
    registry_.register_plugin(plugins_[id].plugin);
    return true;
}

void PluginRuntime::add_bundled() {
    for (auto& plugin : make_bundled_plugins())
        add(std::move(plugin), true);
}

void PluginRuntime::add_external(PluginManager& manager) {
    for (auto& plugin : make_v1_plugin_adapters(manager))
        add(std::move(plugin), /*bundled=*/false);
}

void PluginRuntime::attach_config(const Config& config) {
    live_config_ = &config;
    context_->config = &config;
    services_->config = &config;
    // The host attaches before it starts, so this is startup: fetch the wallet
    // now rather than waiting for the first turn to end. A fresh session should
    // show a balance, not a placeholder.
    request_wallet_refresh();
}

void PluginRuntime::attach_host_services(const HostServices& host) noexcept {
    // Copied by value, not stored by reference: the runtime keeps the pointers
    // the host filled in, so it never depends on the caller's struct outliving
    // this call — only on the services themselves, which the host owns.
    host_services_ = host;
    services_->host = &host_services_;
}

void PluginRuntime::tick() {
    for (const auto& [id, entry] : plugins_) {
        if (registry_.state(id) != PluginRegistry::State::Active)
            continue;
        try {
            entry.plugin->tick();
        } catch (...) {
            // A plugin's tick must never take the host down; the next frame
            // simply shows whatever state it left behind.
        }
    }
    maybe_refresh_wallet();
}

// --- Wallet ---------------------------------------------------------------

namespace {

// Don't let a fast tool loop hammer a provider's endpoint: a turn boundary is
// the trigger, this is the floor.
constexpr long long kWalletRefreshFloorMs = 10LL * 1000;

long long steady_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

bool PluginRuntime::wallet_enabled() const {
    return active_config().wallet_enabled;
}

PluginRuntime::WalletView PluginRuntime::wallet() const {
    const Config& cfg = active_config();
    WalletView view;
    view.enabled = cfg.wallet_enabled;
    view.holder = cfg.provider_name;
    view.supported = wallets_.find(cfg.provider_name) != nullptr;

    const WalletState& state = *wallet_state_;
    // A result counts only when it belongs to the provider that is active now
    // and answers the ticket currently requested. A fetch that lands after a
    // provider switch therefore reads as "not fetched yet" rather than as the
    // previous provider's balance shown under the new one.
    const bool belongs = state.provider == cfg.provider_name;
    const long long active = state.active_ticket.load();
    const bool answered = belongs && active != 0 && state.result_ticket.load() == active;
    // `ready` means there is an amount to show — not merely that the request
    // came back. A provider that declares no wallet answers its ticket with no
    // value, and must read as unavailable rather than as a balance of zero.
    const bool has_value = state.has_value.load();
    view.failed = answered && state.failed.load();
    view.ready = answered && has_value;
    if (view.ready) {
        std::scoped_lock lock(state.mutex);
        view.snapshot = state.snapshot;
    }
    return view;
}

void PluginRuntime::request_wallet_refresh() noexcept {
    wallet_dirty_.store(true);
}

void PluginRuntime::run_wallet_fetch(const std::shared_ptr<WalletState>& state, long long ticket,
                                     const WalletRegistry::Fetch& fetch, const Config& cfg) {
    std::optional<WalletSnapshot> value;
    try {
        value = fetch(cfg);
    } catch (...) {
        // A plugin's fetch is I/O against a third party: a throw is a failed
        // refresh, never a crashed host.
        value = std::nullopt;
    }
    if (value) {
        std::scoped_lock lock(state->mutex);
        state->snapshot = std::move(*value);
    }
    state->has_value.store(value.has_value());
    state->failed.store(!value.has_value());
    state->result_ticket.store(ticket);
    state->inflight.store(false);
}

void PluginRuntime::schedule_wallet_fetch() {
    // Snapshot on THIS thread. The worker must not read the host's live config
    // (the host mutates it) and must not reach back into this object (it may be
    // destroyed while the fetch is still running).
    const Config cfg = active_config();
    const std::string provider = cfg.provider_name;
    const auto state = wallet_state_;
    const long long ticket = ++wallet_ticket_;

    state->provider = provider; // host-thread-only field
    state->active_ticket.store(ticket);
    state->last_ms.store(steady_now_ms());

    const WalletRegistry::Fetch* fetch = wallets_.find(provider);
    if (!fetch) {
        // The active provider declares no wallet: answer the ticket straight
        // away, so the bar shows the unavailable state and no value from the
        // previous provider can survive the switch.
        state->has_value.store(false);
        state->failed.store(false);
        state->result_ticket.store(ticket);
        state->inflight.store(false);
        return;
    }

    state->inflight.store(true);
    WalletRegistry::Fetch work = *fetch; // copy the callable
    // Only the shared state, the ticket, the copied callable and the copied
    // config are captured: nothing here refers to this runtime.
    std::thread([state, ticket, work = std::move(work), cfg] {
        run_wallet_fetch(state, ticket, work, cfg);
    }).detach();
}

void PluginRuntime::perform_wallet_refresh() {
    const Config cfg = active_config();
    const std::string provider = cfg.provider_name;
    const auto state = wallet_state_;
    const long long ticket = ++wallet_ticket_;

    state->provider = provider;
    state->active_ticket.store(ticket);
    state->last_ms.store(steady_now_ms());

    const WalletRegistry::Fetch* fetch = wallets_.find(provider);
    if (!fetch) {
        state->has_value.store(false);
        state->failed.store(false);
        state->result_ticket.store(ticket);
        state->inflight.store(false);
        return;
    }
    state->inflight.store(true);
    run_wallet_fetch(state, ticket, *fetch, cfg);
}

namespace {


// Select the window closest to exhaustion: highest percent_used, ties broken
// by shortest duration (5h beats 7d beats monthly). Returns nullptr when no
// window has a known percent.
int window_duration_rank(const std::string& label) {
    if (label == "5h" || label == "rolling")
        return 0;
    if (label == "7d" || label == "weekly")
        return 1;
    if (label == "24h" || label == "daily")
        return 2;
    return 3; // monthly and everything else is longest
}

const WalletWindow* closest_window(const std::vector<WalletWindow>& ws) {
    const WalletWindow* best = nullptr;
    for (const auto& w : ws) {
        if (w.percent_used < 0)
            continue;
        if (!best || w.percent_used > best->percent_used ||
            (w.percent_used == best->percent_used &&
             window_duration_rank(w.label) < window_duration_rank(best->label)))
            best = &w;
    }
    return best;
}

} // namespace

namespace {

// The bar's answer to "what is left?": the balance when the provider reports
// one, otherwise the window closest to its reset. Wordless, as the bar is.
StatusText wallet_status_text(const WalletSnapshot& snapshot) {
    if (snapshot.credits_balance) {
        const std::string& unit = snapshot.currency.empty() ? std::string("$") : snapshot.currency;
        char buf[48];
        std::snprintf(buf, sizeof(buf), "  %s%.2f", unit.c_str(), *snapshot.credits_balance);
        return StatusText{buf, StatusTone::Dim};
    }
    const WalletWindow* w = closest_window(snapshot.windows);
    if (!w) return StatusText{"  -", StatusTone::Dim};
    char buf[48];
    std::snprintf(buf, sizeof(buf), "  %d%%·%s", static_cast<int>(w->percent_used),
                  w->label.c_str());
    const StatusTone tone = w->percent_used >= 70.0 ? StatusTone::Warn : StatusTone::Dim;
    return StatusText{buf, tone};
}

} // namespace

void PluginRuntime::install_core_ui() {
    std::vector<std::unique_ptr<Capability>> declared = core_status_capabilities();

    // The wallet readout is core, so every provider renders through one path: a
    // plugin supplies a fetch, never a poll loop, a cache and a segment. Highest
    // drop priority, because a balance is the least load-bearing fact on the bar.
    //
    // One readout for one question ("what is left?"). A prepaid balance answers
    // it most directly, so it wins; a provider that meters windows instead gets
    // its closest-expiring window, which is the number a user acts on. Width is
    // the constraint here - `/get provider wallet` carries the whole picture.
    declared.push_back(std::make_unique<StatusSegmentCapability>(
        "wallet", /*priority=*/800, /*drop_priority=*/9,
        [this](const StatusSnapshot&) -> StatusText {
            if (!wallet_enabled())
                return StatusText{};
            const WalletView view = wallet();
            // "-" is the honest answer both for a provider that has no wallet
            // and for one whose last fetch failed; `/get provider wallet`
            // distinguishes the two.
            if (!view.supported || view.failed || !view.ready)
                return StatusText{"  -", StatusTone::Dim};
            return wallet_status_text(view.snapshot);
        }));

    // Installed before any plugin can contribute a panel, so "open the panel
    // view" always lands on the registry.
    declared.push_back(std::make_unique<PanelCapability>(console_panel_spec(*this)));

    services_->set_owner(kCoreOwner);
    for (auto& capability : declared) {
        InstallResult result = capability->install(*services_);
        if (result.declined)
            continue;
        // Core UI failing to install is a harness defect, not a plugin's: fail
        // at startup rather than run with a silently missing status bar.
        if (!result.ok)
            throw std::runtime_error("core UI capability failed to install: " + result.error);
        // Deliberately not recorded in the ledger: that ledger is per plugin and
        // exists to unwind a plugin's contributions. Core UI lives exactly as
        // long as this runtime does, and "core" is not a registered plugin, so
        // there is nothing to unwind and nothing that could disable it.
    }
}

void PluginRuntime::maybe_refresh_wallet() {
    if (!wallet_dirty_.exchange(false))
        return;
    if (wallet_state_->inflight.load()) {
        wallet_dirty_.store(true); // try again once the fetch lands
        return;
    }
    if (steady_now_ms() - wallet_state_->last_ms.load() < kWalletRefreshFloorMs) {
        wallet_dirty_.store(true);
        return;
    }
    schedule_wallet_fetch();
}

void PluginRuntime::start(bool use_persisted_state) {
    for (const auto& [id, entry] : plugins_) {
        if (registry_.state(id) == PluginRegistry::State::Active)
            continue;
        const bool on =
            use_persisted_state ? read_enabled(id, /*default_value=*/entry.bundled) : entry.bundled;
        if (on)
            activate(id);
    }
}

void PluginRuntime::shutdown() {
    for (const auto& [id, entry] : plugins_) {
        if (registry_.state(id) == PluginRegistry::State::Active)
            deactivate(id);
    }
}

std::vector<PluginRuntime::PluginStatus> PluginRuntime::list() const {
    std::vector<PluginStatus> out;
    for (const auto& [id, entry] : plugins_) {
        PluginStatus status;
        status.id = id;
        status.version = entry.plugin->version();
        status.tier = entry.bundled ? "bundled" : "external";
        status.description = entry.plugin->description();
        status.category = entry.plugin->category();
        status.enabled = registry_.state(id) == PluginRegistry::State::Active;
        for (const auto& item : ledger_.contributions(id)) {
            status.contributions.push_back({item.kind, id, item.name, std::string{}});
        }
        out.push_back(std::move(status));
    }
    return out;
}

IPlugin* PluginRuntime::find(const std::string& id) const noexcept {
    auto it = plugins_.find(id);
    return it == plugins_.end() ? nullptr : it->second.plugin.get();
}

bool PluginRuntime::has(const std::string& id) const {
    return plugins_.find(id) != plugins_.end();
}

PluginRuntime::PluginStatus PluginRuntime::status(const std::string& id) const {
    for (auto& status : list()) {
        if (status.id == id)
            return status;
    }
    return {};
}

bool PluginRuntime::set_state(const std::string& id, bool on) {
    if (!has(id))
        return false;
    if (!write_enabled(id, on))
        return false;
    return apply_state(id, on);
}

bool PluginRuntime::apply_state(const std::string& id, bool on) {
    if (!has(id))
        return false;
    return on ? activate(id) : (deactivate(id), true);
}

bool PluginRuntime::activate(const std::string& id) {
    auto it = plugins_.find(id);
    if (it == plugins_.end())
        return false;
    if (registry_.state(id) == PluginRegistry::State::Active)
        return true;
    if (!registry_.activate(id))
        return false;
    if (!install_capabilities(id, *it->second.plugin)) {
        // A plugin that cannot install its capabilities is not active: leaving
        // it half-installed would be the very state the ledger exists to
        // prevent.
        deactivate(id);
        return false;
    }
    return true;
}

void PluginRuntime::deactivate(const std::string& id) {
    registry_.deactivate(id);
    ledger_.unwind(id);
}

bool PluginRuntime::install_capabilities(const std::string& id, IPlugin&) {
    auto it = plugins_.find(id);
    if (it == plugins_.end())
        return false;
    services_->set_owner(id);
    // The capabilities declared at registration are the ones installed; a
    // plugin hands them over once and the runtime owns them from then on.
    for (auto& capability : it->second.declared) {
        if (!capability)
            continue;
        InstallResult result = capability->install(*services_);
        if (result.declined)
            continue; // nothing to install is not a reason to fail the plugin
        if (!result.ok)
            return false;
        ledger_.record(id, std::move(result.contribution));
    }
    return true;
}

std::vector<AuditFinding> PluginRuntime::audit() const {
    return audit_toolset(services_->tools());
}

std::vector<ExtensionItem> PluginRuntime::contributions() const {
    // Ledger-driven on purpose. Enumerating registries by hand is how this
    // drifts: the first version of this function forgot status segments,
    // panels and wallets, so the "nothing survives disable" invariant was
    // weaker than it looked. The ledger records every install by definition.
    std::vector<ExtensionItem> out;
    for (const auto& plugin : list()) {
        out.insert(out.end(), plugin.contributions.begin(), plugin.contributions.end());
    }
    return out;
}

} // namespace agent
