
#include "agent/search_backend.h"

namespace agent {

SearchBackendRegistry& SearchBackendRegistry::instance() {
    static SearchBackendRegistry reg;
    return reg;
}

void SearchBackendRegistry::register_backend(
    const std::string& mode,
    std::function<std::unique_ptr<SearchBackend>()> factory) {
    factories_[mode] = std::move(factory);
}

std::unique_ptr<SearchBackend>
SearchBackendRegistry::create(const std::string& mode) const {
    auto it = factories_.find(mode);
    if (it != factories_.end()) return it->second();
    // Fallback for static archive linking: if the static initializers
    // in grep_backend.o/semantic_backend.o didn't run (the linker didn't
    // pull them in because no other symbol references them), try the
    // factory functions directly.
    if (mode == "grep") return make_grep_backend();
    if (mode == "semantic") return make_semantic_backend();
    return nullptr;
}

std::vector<std::string> SearchBackendRegistry::available() const {
    std::vector<std::string> modes;
    modes.reserve(factories_.size());
    for (const auto& [mode, _] : factories_) modes.push_back(mode);
    if (modes.empty()) return {"grep", "semantic"};
    return modes;
}

} // namespace agent
