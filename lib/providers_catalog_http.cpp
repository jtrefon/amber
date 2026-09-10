
#include "agent/config.h"
#include "agent/model_probe.h"
#include "agent/providers.h"

// Adapter: model listing through the provider's own dialect.
//
// The provider carries its flavor, so the probe uses the right endpoint, the
// right auth header, and the right parser. Previously this built a bare config
// and always spoke OpenAI: /provider test and model listing were silently
// wrong for every non-OpenAI provider (a Gemini or Anthropic endpoint probed
// at /models with a Bearer token).

namespace agent {

namespace {

class HttpModelCatalog : public ModelCatalog {
public:
    std::vector<std::string> list_models(const Provider& p) const override {
        Config cfg;
        cfg.api_base = p.api_base;
        cfg.api_key = p.api_key;
        cfg.flavor = p.flavor;
        std::vector<std::string> ids;
        for (const auto& m : list_model_info(cfg)) ids.push_back(m.id);
        return ids;
    }
};

} // namespace

std::unique_ptr<ModelCatalog> make_http_model_catalog() {
    return std::make_unique<HttpModelCatalog>();
}

} // namespace agent
