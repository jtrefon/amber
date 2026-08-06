
#include <cstdlib>

#include "agent/config.h"
#include "agent/providers.h"

// Adapter: the user-defined "custom" provider. Its connection comes from
// the user's own sources, never from the active provider's runtime state:
//   env AMBER_API_BASE / AMBER_API_KEY / AMBER_MODEL
//   amber.conf in the working directory
//   the global config — only when it names provider "custom"
// The repository re-reads those sources on every lookup, so edits are
// picked up without restarts. Saving persists a "custom" entry in the
// global config.

#include <filesystem>
#include <fstream>

namespace agent {

namespace {

const char* env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : fallback;
}

// Layered user connection: env > amber.conf > global (custom only).
bool user_connection(Config& out) {
    // The Config DTO carries defaults (api_base=localhost:8000,
    // model=gpt-4o-mini) that must never count as a user connection —
    // start from empty so only real sources resolve.
    out.api_base.clear();
    out.api_key.clear();
    out.model.clear();
    Config scratch;
    scratch.api_base.clear();
    scratch.api_key.clear();
    scratch.model.clear();
    std::ifstream gc(global_config_path());
    if (gc) scratch.load(global_config_path());
    if (scratch.provider_name.empty() || scratch.provider_name == "custom") {
        out.api_base = scratch.api_base;
        out.api_key = scratch.api_key;
        out.model = scratch.model;
    }
    Config proj_holder;
    proj_holder.api_base.clear();
    proj_holder.api_key.clear();
    proj_holder.model.clear();
    std::ifstream ac("amber.conf");
    if (ac) {
        proj_holder.load("amber.conf");
        if (!proj_holder.api_base.empty()) out.api_base = proj_holder.api_base;
        if (!proj_holder.api_key.empty()) out.api_key = proj_holder.api_key;
        if (!proj_holder.model.empty()) out.model = proj_holder.model;
    }
    const char* base = env_or("AMBER_API_BASE", nullptr);
    if (base) out.api_base = base;
    const char* key = env_or("AMBER_API_KEY", nullptr);
    if (key) out.api_key = key;
    const char* model = env_or("AMBER_MODEL", nullptr);
    if (model) out.model = model;
    return !out.api_base.empty();
}

Provider from_config(const Config& c) {
    Provider p;
    p.name = "custom";
    p.api_base = c.api_base;
    p.api_key = c.api_key;
    p.default_model = c.model;
    p.requires_key = false;
    p.builtin = false;
    return p;
}

class CustomProviderRepository : public ProviderRepository {
public:
    std::vector<Provider> all() const override {
        Config c;
        if (!user_connection(c)) return {};
        return {from_config(c)};
    }

    std::optional<Provider> find(const std::string& name) const override {
        if (name != "custom") return std::nullopt;
        Config c;
        if (!user_connection(c)) return std::nullopt;
        return from_config(c);
    }

    bool save(const Provider& p) override {
        if (p.name != "custom") return false;
        Config c;
        c.provider_name = "custom";
        c.api_base = p.api_base;
        c.api_key = p.api_key;
        c.model = p.default_model;
        return c.save_global(global_config_path());
    }

    bool remove(const std::string&) override { return false; }
};

} // namespace

std::unique_ptr<ProviderRepository> make_custom_provider_repository() {
    return std::make_unique<CustomProviderRepository>();
}

} // namespace agent
