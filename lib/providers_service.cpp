
#include "agent/providers.h"

#include <algorithm>

#include "agent/config.h"

namespace agent {

namespace {

const std::vector<std::pair<std::string, ProviderCapabilities>>&
capability_overrides() {
    static const std::vector<std::pair<std::string, ProviderCapabilities>>
        table = {
            // OpenRouter passes reasoning_effort through to upstreams.
            {"openrouter", {true, true, "openai"}},
            {"kilocode", {true, true, "openai"}},
        };
    return table;
}

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

ProviderCapabilities ProviderService::capabilities(
    const std::string& name) const {
    for (const auto& [n, caps] : capability_overrides())
        if (n == name) return caps;
    return ProviderCapabilities{};
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
}

bool seed_custom_provider(const Config& connection) {
    Provider p;
    p.name = "custom";
    p.api_base = connection.api_base;
    p.api_key = connection.api_key;
    p.default_model = connection.model;
    p.default_context_size = connection.context_size;
    return make_file_provider_repository()->save(p);
}

std::unique_ptr<ProviderService> make_default_provider_service(
    const Config&) {
    std::vector<std::unique_ptr<ProviderRepository>> repos;
    repos.push_back(make_static_provider_repository());
    repos.push_back(make_file_provider_repository());
    return std::make_unique<ProviderService>(
        std::move(repos), make_http_model_catalog());
}

} // namespace agent
