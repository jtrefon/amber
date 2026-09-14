#include "search_semantic_plugin.h"

#include "agent/search_backend.h"

namespace agent::plugins {

std::vector<std::unique_ptr<Capability>> make_semantic_backend_capabilities() {
    std::vector<std::unique_ptr<Capability>> caps;
    caps.push_back(std::make_unique<SearchBackendCapability>(
        "semantic", [] { return make_semantic_backend(); }));
    return caps;
}

} // namespace agent::plugins
