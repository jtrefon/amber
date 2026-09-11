#include "agent/plugin_runtime.h"

#include "agent/core_segments.h"
#include "agent/dialect.h"
#include "agent/plugin_console.h"
#include "agent/plugins_bundled.h"
#include "agent/plugin_v1_adapter.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <utility>

namespace agent {

namespace {

namespace fs = std::filesystem;

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
    : tools_(&tools), config_(std::move(config)), workspace_(&workspace) {
    // Amber's own segments are registry entries like any other, so the bar is
    // composed from one list whether a segment comes from the core or a plugin.
    register_core_status_segments(status_);
    // The console is registered before any plugin can contribute a panel, so
    // "open the panel view" always lands on the registry.
    register_console_panel(panels_, *this);
    services_ = std::make_unique<PluginServices>(tools, prompts_, commands_, status_, panels_,
                                                 wallets_, settings_, bus_);
    context_ = std::make_unique<PluginContext>(PluginContext{bus_, tools, &config_, *workspace_});
    services_->config = &config_;
    registry_.set_context(context_.get());

    // The wallet belongs to the active provider and is refreshed when a turn
    // ends rather than on a timer: a balance only moves because we spent
    // something.
    wallet_turn_sub_ = Events(bus_).subscribe<TurnEndedEvent>(
        [this](const TurnEndedEvent&) { request_wallet_refresh(); });

    // The readout is core, so every provider renders through one path: a plugin
    // supplies a fetch, never a poll loop, a cache and a segment.
    register_wallet_segment();
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
    WalletView view;
    view.enabled = wallet_enabled();
    const std::string provider = active_config().provider_name;
    view.holder = provider;
    view.supported = wallets_.find(provider) != nullptr;
    view.ready = wallet_ready_.load();
    view.failed = wallet_failed_.load();
    view.amount = wallet_amount_.load();
    return view;
}

void PluginRuntime::request_wallet_refresh() noexcept {
    wallet_dirty_.store(true);
}

void PluginRuntime::perform_wallet_refresh() {
    const Config& cfg = active_config();
    const WalletRegistry::Fetch* fetch = wallets_.find(cfg.provider_name);
    if (!fetch) {
        // The active provider declares no wallet: nothing to report, and no
        // stale value from a previous provider may survive.
        wallet_ready_.store(false);
        wallet_failed_.store(false);
        wallet_last_ms_.store(steady_now_ms());
        return;
    }
    std::optional<double> value;
    try {
        value = (*fetch)(cfg);
    } catch (...) {
        // A plugin's fetch is I/O against a third party: a throw is a failed
        // refresh, never a crashed host.
        value = std::nullopt;
    }
    if (value) {
        wallet_amount_.store(*value);
        wallet_ready_.store(true);
        wallet_failed_.store(false);
    } else {
        wallet_ready_.store(false);
        wallet_failed_.store(true);
    }
    wallet_last_ms_.store(steady_now_ms());
}

void PluginRuntime::register_wallet_segment() {
    // Highest drop priority: the wallet is the first thing to give up its
    // columns when the terminal is narrow, because it is the least load-bearing
    // fact on the bar.
    status_.add("", "wallet", /*priority=*/800, /*drop_priority=*/9,
                [this](const StatusSnapshot&) -> StatusText {
                    if (!wallet_enabled())
                        return StatusText{};
                    const WalletView view = wallet();
                    // "-" is the honest answer both for a provider that has no
                    // wallet and for one whose last fetch failed; `/get
                    // provider wallet` distinguishes the two.
                    if (!view.supported || view.failed || !view.ready)
                        return StatusText{"  -", StatusTone::Dim};
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "  $%.2f", view.amount);
                    return StatusText{buf, StatusTone::Dim};
                });
}

void PluginRuntime::maybe_refresh_wallet() {
    if (!wallet_dirty_.exchange(false))
        return;
    if (wallet_inflight_.load()) {
        wallet_dirty_.store(true); // try again once the fetch lands
        return;
    }
    if (steady_now_ms() - wallet_last_ms_.load() < kWalletRefreshFloorMs) {
        wallet_dirty_.store(true);
        return;
    }
    wallet_dirty_.store(false);
    wallet_inflight_.store(true);
    // The fetch is the plugin's I/O; the UI thread only schedules it.
    std::thread([this] {
        perform_wallet_refresh();
        wallet_inflight_.store(false);
    }).detach();
}

IPlugin* PluginRuntime::find(const std::string& id) const noexcept {
    auto it = plugins_.find(id);
    return it == plugins_.end() ? nullptr : it->second.plugin.get();
}

void PluginRuntime::start() {
    for (const auto& [id, entry] : plugins_) {
        if (registry_.state(id) == PluginRegistry::State::Active)
            continue;
        if (read_enabled(id, /*default_value=*/entry.bundled)) {
            activate(id);
        }
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
        if (!result.ok)
            return false;
        ledger_.record(id, std::move(result.contribution));
    }
    return true;
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
