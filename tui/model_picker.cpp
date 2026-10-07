#include "tui/model_picker.h"

#include <agent/config.h>
#include <agent/model_probe.h>

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

std::vector<ProviderCatalog> catalogs_from(const std::vector<ProviderEndpoint>& endpoints,
                                           CatalogReader read) {
    std::vector<ProviderCatalog> out;
    out.reserve(endpoints.size());
    for (const auto& e : endpoints) {
        ProviderCatalog c;
        c.provider = e.name;
        c.configured = e.has_endpoint;
        if (c.configured)
            c.models = read(e);
        out.push_back(std::move(c));
    }
    return out;
}

nlohmann::json model_subtree(const std::vector<ProviderModel>& rows, const std::string& prefix) {
    nlohmann::json subtree = nlohmann::json::object();
    for (const auto& m : rows) {
        nlohmann::json& leaf =
            subtree["set"]["children"]["model"]["children"][provider_key(m.provider, m.id)];
        leaf["action"] = prefix + provider_key(m.provider, m.id);
        if (m.context > 0)
            leaf["help"] = "ctx " + std::to_string(m.context);
    }
    return subtree;
}

std::vector<ProviderModel> cached_models_for(const ProviderEndpoint& e) {
    agent::Config cfg;
    cfg.api_base = e.api_base;
    cfg.flavor = e.flavor;
    std::vector<ProviderModel> out;
    for (const auto& m : agent::list_model_info_cached(cfg)) {
        ProviderModel pm;
        pm.provider = e.name;
        pm.id = m.id;
        pm.context = m.context ? m.context : m.context_train;
        out.push_back(pm);
    }
    return out;
}

std::string provider_list_line(const std::string& name, const std::string& api_base, bool active,
                               bool requires_key, bool has_key) {
    const bool has_endpoint = !api_base.empty();
    const char mark = !has_endpoint ? ' ' : (requires_key && !has_key ? '!' : 'x');
    std::string line = "  ";
    line += active ? '*' : ' ';
    line += "  [";
    line += mark;
    line += "]  ";
    line += name;
    line += "  (";
    if (!has_endpoint) {
        line += "unconfigured";
    } else if (requires_key && !has_key) {
        line += "no API key";
    } else {
        line += api_base;
    }
    line += ")";
    return line;
}

} // namespace tui