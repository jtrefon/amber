#include "agent/plugin_runtime.h"

#include "agent/plugins_bundled.h"

#include <filesystem>
#include <fstream>

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
    if (!f) return default_value;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("enabled=", 0) != 0) continue;
        const std::string value = line.substr(8);
        return value == "1" || value == "true" || value == "on";
    }
    return default_value;
}

bool write_enabled(const std::string& id, bool enabled) {
    std::error_code ec;
    fs::create_directories(plugin_dir(id), ec);
    std::ofstream f(state_path(id), std::ios::trunc);
    if (!f) return false;
    f << "# amber plugin state: " << id << "\n";
    f << "enabled=" << (enabled ? 1 : 0) << "\n";
    return static_cast<bool>(f);
}

} // namespace

PluginRuntime::PluginRuntime(ToolRegistry& tools, const Config& config,
                             const Workspace& workspace)
    : tools_(&tools), config_(config), workspace_(&workspace) {
    services_ = std::make_unique<PluginServices>(tools, prompts_, commands_,
                                                 settings_, bus_);
    context_ = std::make_unique<PluginContext>(
        PluginContext{bus_, tools, config_, workspace});
    registry_.set_context(context_.get());
}

PluginRuntime::~PluginRuntime() {
    shutdown();
}

void PluginRuntime::add(std::shared_ptr<IPlugin> plugin, bool bundled) {
    if (!plugin) return;
    const std::string id = plugin->id();
    plugins_[id] = Entry{std::move(plugin), bundled};
    registry_.register_plugin(plugins_[id].plugin);
}

void PluginRuntime::add_bundled() {
    for (auto& plugin : make_bundled_plugins()) add(std::move(plugin), true);
}

void PluginRuntime::start() {
    for (const auto& [id, entry] : plugins_) {
        if (registry_.state(id) == PluginRegistry::State::Active) continue;
        if (read_enabled(id, /*default_value=*/entry.bundled)) {
            activate(id);
        }
    }
}

void PluginRuntime::shutdown() {
    for (const auto& [id, entry] : plugins_) {
        if (registry_.state(id) == PluginRegistry::State::Active) deactivate(id);
    }
}

std::vector<PluginRuntime::PluginStatus> PluginRuntime::list() const {
    std::vector<PluginStatus> out;
    for (const auto& [id, entry] : plugins_) {
        PluginStatus status;
        status.id = id;
        status.version = entry.plugin->version();
        status.tier = entry.bundled ? "bundled" : "external";
        status.enabled =
            registry_.state(id) == PluginRegistry::State::Active;
        for (const auto& item : ledger_.contributions(id)) {
            status.contributions.push_back(
                {item.kind, id, item.name, std::string{}});
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
        if (status.id == id) return status;
    }
    return {};
}

bool PluginRuntime::set_state(const std::string& id, bool on) {
    if (!has(id)) return false;
    if (!write_enabled(id, on)) return false;
    return on ? activate(id) : (deactivate(id), true);
}

bool PluginRuntime::activate(const std::string& id) {
    auto it = plugins_.find(id);
    if (it == plugins_.end()) return false;
    if (registry_.state(id) == PluginRegistry::State::Active) return true;
    if (!registry_.activate(id)) return false;
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

bool PluginRuntime::install_capabilities(const std::string& id, IPlugin& plugin) {
    services_->set_owner(id);
    for (auto& capability : plugin.capabilities()) {
        if (!capability) continue;
        InstallResult result = capability->install(*services_);
        if (!result.ok) return false;
        ledger_.record(id, std::move(result.contribution));
    }
    return true;
}

std::vector<ExtensionItem> PluginRuntime::contributions() const {
    std::vector<ExtensionItem> out;
    const auto append = [&out](const std::vector<ExtensionItem>& items) {
        out.insert(out.end(), items.begin(), items.end());
    };
    append(prompts_.items());
    append(commands_.items());
    append(settings_.items());
    return out;
}

} // namespace agent
