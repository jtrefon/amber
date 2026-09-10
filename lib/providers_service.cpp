
#include "agent/providers.h"

#include <algorithm>
#include <mutex>

#include "agent/config.h"

namespace agent {

namespace {

// Plugin-contributed presets. Guarded because a provider plugin can be toggled
// while the provider list is being read (spec invariant 12).
std::mutex& preset_mutex() {
    static std::mutex mtx;
    return mtx;
}

std::vector<std::pair<Provider, std::string>>& preset_table() {
    static std::vector<std::pair<Provider, std::string>> table;  // preset, owner
    return table;
}

} // namespace

// Presets contributed by plugins (defined here so the service and the
// repository below share one table).
void register_provider_preset(const Provider& preset, const std::string& owner) {
    std::scoped_lock lock(preset_mutex());
    for (auto& entry : preset_table()) {
        if (entry.first.name == preset.name) {
            entry = {preset, owner};
            return;
        }
    }
    preset_table().push_back({preset, owner});
}

void unregister_provider_presets_for(const std::string& owner) {
    if (owner.empty()) return;
    std::scoped_lock lock(preset_mutex());
    auto& table = preset_table();
    table.erase(std::remove_if(table.begin(), table.end(),
                               [&](const std::pair<Provider, std::string>& e) {
                                   return e.second == owner;
                               }),
                table.end());
}

std::vector<Provider> plugin_provider_presets() {
    std::scoped_lock lock(preset_mutex());
    std::vector<Provider> out;
    for (const auto& entry : preset_table()) out.push_back(entry.first);
    return out;
}

namespace {

// Adapter: plugin-contributed presets as a repository, so they merge into the
// provider list exactly like the built-in and file layers.
class PluginProviderRepository : public ProviderRepository {
public:
    std::vector<Provider> all() const override { return plugin_provider_presets(); }

    std::optional<Provider> find(const std::string& name) const override {
        for (const auto& p : plugin_provider_presets())
            if (p.name == name) return p;
        return std::nullopt;
    }

    // Read-only layer: plugin presets come from the plugin, not from here.
    bool save(const Provider&) override { return false; }
    bool remove(const std::string&) override { return false; }
};

} // namespace

ProviderService::ProviderService(
    std::vector<std::unique_ptr<ProviderRepository>> repos,
    std::unique_ptr<ModelCatalog> catalog)
    : repos_(std::move(repos)), catalog_(std::move(catalog)) {}

ProviderService::~ProviderService() = default;

std::vector<Provider> ProviderService::available() const {
    // Later repositories override earlier ones on name collisions.
    std::vector<Provider> out;
    for (const auto& repo : repos_) {
        for (const auto& p : repo->all()) {
            auto it = std::find_if(out.begin(), out.end(),
                                   [&](const Provider& e) {
                                       return e.name == p.name;
                                   });
            if (it == out.end()) {
                out.push_back(p);
            } else {
                *it = p;
            }
        }
    }
    return out;
}

std::optional<Provider> ProviderService::find(const std::string& name) const {
    std::optional<Provider> found;
    for (const auto& repo : repos_) {
        if (auto p = repo->find(name)) found = *p;
    }
    return found;
}

ProviderSelection ProviderService::select(const std::string& name) const {
    ProviderSelection sel;
    const auto p = find(name);
    if (!p) {
        sel.error = "unknown provider: " + name;
        return sel;
    }
    sel.provider = *p;
    if (p->api_base.empty()) {
        sel.error = "provider '" + name +
                    "' has no endpoint configured (set one via /provider "
                    "add or amber.conf)";
        return sel;
    }
    if (p->requires_key && p->api_key.empty())
        sel.warning = "provider '" + name + "' requires an API key";
    return sel;
}

bool ProviderService::save(const Provider& p) {
    for (auto it = repos_.rbegin(); it != repos_.rend(); ++it) {
        if ((*it)->save(p)) return true;
    }
    return false;
}

bool ProviderService::remove(const std::string& name) {
    bool removed = false;
    for (auto& repo : repos_)
        if (repo->find(name)) removed |= repo->remove(name);
    return removed;
}

bool ProviderService::remember_model(const std::string& provider,
                                     const std::string& model) {
    const auto p = find(provider);
    if (!p) return false;
    Provider updated = *p;
    updated.default_model = model;
    return save(updated);
}

bool ProviderService::validate(const std::string& name) {
    const auto p = find(name);
    if (!p || p->api_base.empty()) return false;
    return !catalog_->list_models(*p).empty();
}

void apply_selection(Config& cfg, const ProviderSelection& sel) {
    cfg.provider_name = sel.provider.name;
    if (!sel.provider.api_base.empty()) cfg.api_base = sel.provider.api_base;
    // A key from the provider file travels with the switch; a provider
    // without its own key keeps whatever the user configured (env/global).
    if (!sel.provider.api_key.empty()) cfg.api_key = sel.provider.api_key;
    // Auto-load the last-used model for this provider.
    if (!sel.provider.default_model.empty()) {
        cfg.model = sel.provider.default_model;
        cfg.model_explicit = true;
    }
    if (sel.provider.default_context_size > 0 && !cfg.context_explicit)
        cfg.context_size = sel.provider.default_context_size;
    // The dialect the client resolves travels with the provider, so a provider
    // contributed by a plugin needs nothing from a table in the core. Derived
    // on every selection, never persisted.
    cfg.flavor = sel.provider.flavor;
}

bool seed_provider(const std::string& name, const Config& connection) {
    if (name.empty()) return false;
    Provider p;
    p.name = name;
    p.api_base = connection.api_base;
    p.api_key = connection.api_key;
    p.default_model = connection.model;
    p.default_context_size = connection.context_size;
    return make_file_provider_repository()->save(p);
}

std::unique_ptr<ProviderService> make_default_provider_service(
    const Config&) {
    std::vector<std::unique_ptr<ProviderRepository>> repos;
    // Every provider definition comes from a plugin; the file layer holds what
    // the user wrote and overrides a same-named preset (later repos win).
    repos.push_back(std::make_unique<PluginProviderRepository>());
    repos.push_back(make_file_provider_repository());
    return std::make_unique<ProviderService>(
        std::move(repos), make_http_model_catalog());
}

} // namespace agent
