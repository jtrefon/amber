#include "search_grep_plugin.h"

#include "agent/search_backend.h"

namespace agent::plugins {

std::vector<std::unique_ptr<Capability>> make_grep_backend_capabilities() {
    std::vector<std::unique_ptr<Capability>> caps;
    caps.push_back(std::make_unique<SearchBackendCapability>("grep", [] {
        return make_grep_backend();
    }));
    return caps;
}

} // namespace agent::plugins
