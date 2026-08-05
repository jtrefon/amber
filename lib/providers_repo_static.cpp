
#include "agent/providers.h"

// Adapter: the well-known presets as code constants. Only here do
// provider names/endpoints exist in code; the domain never sees them.

namespace agent {

namespace {

class StaticProviderRepository : public ProviderRepository {
public:
    std::vector<Provider> all() const override {
        std::vector<Provider> out;
        out.push_back({"openrouter",
                       "https://openrouter.ai/api/v1", "", true,
                       "openai/gpt-4o", 0, /*builtin=*/true});
        out.push_back({"kilocode",
                       "https://api.kilo.ai/api/gateway", "", true,
                       "kilo-auto/free", 0, /*builtin=*/true});
        return out;
    }

    std::optional<Provider> find(const std::string& name) const override {
        for (const auto& p : all())
            if (p.name == name) return p;
        return std::nullopt;
    }

    bool save(const Provider&) override { return false; }
    bool remove(const std::string&) override { return false; }
};

} // namespace

std::unique_ptr<ProviderRepository> make_static_provider_repository() {
    return std::make_unique<StaticProviderRepository>();
}

} // namespace agent
