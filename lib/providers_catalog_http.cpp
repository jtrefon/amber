
#include "agent/config.h"
#include "agent/model_probe.h"
#include "agent/providers.h"

// Adapter: OpenAI-compatible model listing via the /v1/models probe.
// Reuses the existing probe/parse machinery; only the provider data is
// turned into a transport Config here.

namespace agent {

namespace {

class HttpModelCatalog : public ModelCatalog {
public:
    std::vector<std::string> list_models(const Provider& p) const override {
        Config cfg;
        cfg.api_base = p.api_base;
        cfg.api_key = p.api_key;
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
