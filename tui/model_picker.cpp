#include "tui/model_picker.h"

#include <algorithm>

namespace tui {

namespace {

// First occurrence of the separator, so a model id containing "::" further on
// cannot steal the split.
std::size_t sep_pos(const std::string& key) {
    return key.find(kProviderSep);
}

} // namespace

bool split_provider_key(const std::string& key, std::string& provider, std::string& id) {
    const std::size_t p = sep_pos(key);
    if (p == std::string::npos || p == 0)
        return false;
    provider = key.substr(0, p);
    id = key.substr(p + std::char_traits<char>::length(kProviderSep));
    return true;
}

std::string provider_key(const std::string& provider, const std::string& id) {
    return provider + kProviderSep + id;
}

std::vector<ProviderModel> aggregate_provider_models(const std::vector<ProviderCatalog>& catalogs,
                                                     const std::string& active_provider) {
    std::vector<std::size_t> order;
    order.reserve(catalogs.size());
    for (std::size_t i = 0; i < catalogs.size(); ++i) {
        if (!catalogs[i].configured || catalogs[i].models.empty())
            continue;
        if (catalogs[i].provider == active_provider)
            order.insert(order.begin(), i); // active provider's block leads
        else
            order.push_back(i);
    }
    std::vector<ProviderModel> out;
    for (const std::size_t i : order)
        out.insert(out.end(), catalogs[i].models.begin(), catalogs[i].models.end());
    return out;
}

} // namespace tui