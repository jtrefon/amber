#include "agent/global_config.h"

#include <fstream>

#include "agent/config_paths.h"
#include "agent/providers.h"

namespace agent {

GlobalConfig apply_global_selection(Config& cfg) {
    GlobalConfig global;
    std::ifstream in(global_config_path());
    if (!in)
        return global;
    global.cfg.load(global_config_path());
    global.loaded = true;
    if (global.cfg.context_explicit && global.cfg.context_size > 0) {
        cfg.context_size = global.cfg.context_size;
        cfg.context_explicit = true;
    }
    // Provider-scoped runtime knobs the provider file does not carry. They are
    // global because they describe the account and the model, so both hosts must
    // see them; the default is not copied, so a future default can move.
    if (global.cfg.prompt_cache_ttl != "5m")
        cfg.prompt_cache_ttl = global.cfg.prompt_cache_ttl;
    // The provider domain owns the endpoint and the key: a provider's last-used
    // model comes with the selection, which is why the global model is applied
    // as a separate, later step.
    auto providers = make_default_provider_service(cfg);
    if (!global.cfg.provider_name.empty()) {
        auto selection = providers->select(global.cfg.provider_name);
        if (selection.ok())
            apply_selection(cfg, selection);
    }
    return global;
}

void apply_global_model(Config& cfg, const GlobalConfig& global) {
    if (!global.loaded || global.cfg.model.empty() || !global.cfg.model_explicit)
        return;
    cfg.model = global.cfg.model;
    cfg.model_explicit = true;
}

} // namespace agent
