
#ifndef AGENT_PROVIDERS_H
#define AGENT_PROVIDERS_H

// Provider domain: agnostic provider handling in the core.
//
// The domain knows providers as data (Provider) and provider-specific
// differences as capabilities; persistence and network access happen
// behind ports (ProviderRepository, ModelCatalog). Callers (CLI/TUI)
// depend on ProviderService only — never on files, HTTP, or provider
// names. Wiring of adapters happens at the boundary
// (make_default_provider_service).

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace agent {

struct Config;

// ---------------------------------------------------------------------------
// Domain entity — a provider definition. Pure data; owned by repositories.
// ---------------------------------------------------------------------------

struct Provider {
    std::string name;
    std::string api_base;
    std::string api_key;       // may be empty (env / none)
    bool requires_key = false;
    std::string default_model; // last-used model for this provider
    int default_context_size = 0;
    bool builtin = false;      // code preset vs user-added file
};

// Per-provider differences from the OpenAI-compatible baseline. This is
// the only place provider-specific behavior is declared; callers never
// branch on provider names.
struct ProviderCapabilities {
    bool bearer_auth = true;                 // Authorization: Bearer <key>
    bool supports_reasoning_effort = false;  // o-series / vLLM / DeepSeek
    std::string flavor = "openai";           // future differences
};

// ---------------------------------------------------------------------------
// Ports — implemented by adapters, never by the domain.
// ---------------------------------------------------------------------------

// Persistence + discovery of provider definitions.
class ProviderRepository {
public:
    virtual ~ProviderRepository() = default;
    virtual std::vector<Provider> all() const = 0;
    virtual std::optional<Provider> find(const std::string& name) const = 0;
    virtual bool save(const Provider& p) = 0;
    virtual bool remove(const std::string& name) = 0;
};

// Model listing for a provider endpoint.
class ModelCatalog {
public:
    virtual ~ModelCatalog() = default;
    virtual std::vector<std::string> list_models(const Provider& p) const = 0;
};

// ---------------------------------------------------------------------------
// Application service — the single entry point for provider handling.
// ---------------------------------------------------------------------------

struct ProviderSelection {
    Provider provider;
    std::string error;    // non-empty on hard failure
    std::string warning;  // non-empty on soft issues (e.g. missing key)
    bool ok() const noexcept { return error.empty(); }
};

class ProviderService {
public:
    // Repositories are consulted in order; a later repository overrides an
    // earlier one on name collisions. The caller owns nothing after
    // construction (the service owns the adapters).
    ProviderService(std::vector<std::unique_ptr<ProviderRepository>> repos,
                    std::unique_ptr<ModelCatalog> catalog);
    ~ProviderService();
    ProviderService(const ProviderService&) = delete;
    ProviderService& operator=(const ProviderService&) = delete;

    // Merged, de-duplicated list (later repositories override).
    std::vector<Provider> available() const;
    std::optional<Provider> find(const std::string& name) const;

    // Resolve a provider and validate it for use. Auto-loads the
    // provider's last-used model (default_model). Never falls back
    // silently: unknown names and unconfigured endpoints are errors.
    ProviderSelection select(const std::string& name) const;

    // Persist a provider definition (create/update; user data lands in
    // the file layer). Returns false when no repository accepts it.
    bool save(const Provider& p);
    // Remove a saved provider definition.
    bool remove(const std::string& name);

    // Persist the last-used model for a provider (default_model).
    bool remember_model(const std::string& provider, const std::string& model);

    // Provider-specific differences (defaults + override table).
    ProviderCapabilities capabilities(const std::string& name) const;

    // Reachability probe via the model catalog.
    bool validate(const std::string& name);

private:
    std::vector<std::unique_ptr<ProviderRepository>> repos_;
    std::unique_ptr<ModelCatalog> catalog_;
};

// Apply a selection to a transport Config: provider name, endpoint, key
// (non-empty fields only) and the provider's last-used model.
void apply_selection(Config& cfg, const ProviderSelection& sel);

// ---------------------------------------------------------------------------
// Adapter factories (infra). Hosts normally use make_default_provider_
// service; the individual factories exist for custom wiring and tests.
// ---------------------------------------------------------------------------

// Well-known presets (openrouter, kilocode) as code constants.
std::unique_ptr<ProviderRepository> make_static_provider_repository();
// ~/.config/amber/providers/*.conf persistence (user-added providers).
std::unique_ptr<ProviderRepository> make_file_provider_repository();
// The user-defined "custom" connection: env AMBER_API_* / amber.conf /
// the global config (only when it names provider "custom").
std::unique_ptr<ProviderRepository> make_custom_provider_repository();
// OpenAI-compatible /v1/models probe.
std::unique_ptr<ModelCatalog> make_http_model_catalog();

// Boundary wiring: static presets + user files + the custom connection.
std::unique_ptr<ProviderService> make_default_provider_service(
    const Config& cfg);

} // namespace agent

#endif // AGENT_PROVIDERS_H
