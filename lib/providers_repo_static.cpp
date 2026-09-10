
#include "agent/providers.h"

// Adapter: the well-known presets as code constants. Only here do
// provider names/endpoints exist in code; the domain never sees them.

namespace agent {

namespace {

class StaticProviderRepository : public ProviderRepository {
public:
    std::vector<Provider> all() const override {
        std::vector<Provider> out;
        // The named providers (openrouter, kilocode, anthropic, gemini) are
        // contributed by their plugins: each owns its preset, so switching one
        // off removes it. What remains here is the one provider that is not a
        // vendor — the user's own endpoint, configured via
        // ~/.config/amber/providers/custom.conf.
        out.push_back({"custom", "", "", false, "", 0, /*builtin=*/true});
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
