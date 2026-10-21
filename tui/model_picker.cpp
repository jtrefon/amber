#include "tui/model_picker.h"

#include <agent/config.h>
#include <agent/model_probe.h>

#include <algorithm>
#include <utility>

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
        c.configured = !e.api_base.empty();
        if (c.configured)
            c.models = read(e);
        out.push_back(std::move(c));
    }
    return out;
}

std::string compact_tokens(int tokens) {
    if (tokens <= 0)
        return {};
    if (tokens >= 1000000) {
        const int whole = tokens / 1000000;
        const int tenth = (tokens % 1000000) / 100000;
        return std::to_string(whole) + (tenth ? "." + std::to_string(tenth) : "") + "M";
    }
    if (tokens >= 1000)
        return std::to_string(tokens / 1000) + "k"; // 16384 -> 16k, 128000 -> 128k
    return std::to_string(tokens);
}

nlohmann::json model_subtree(const std::vector<ProviderModel>& rows, const std::string& prefix) {
    nlohmann::json subtree = nlohmann::json::object();
    for (const auto& m : rows) {
        nlohmann::json& leaf =
            subtree["set"]["children"]["model"]["children"][provider_key(m.provider, m.id)];
        leaf["action"] = prefix + provider_key(m.provider, m.id);
        std::string help;
        // ASCII only: the drawer is rendered in terminals that do not all agree
        // on UTF-8, and a middle dot arrives as M-BM-7 on a non-UTF-8 locale.
        const auto add = [&help](const std::string& part) {
            if (!help.empty())
                help += " | ";
            help += part;
        };
        if (m.context > 0)
            add("in " + compact_tokens(m.context));
        if (m.max_output > 0)
            add("out " + compact_tokens(m.max_output));
        if (m.thinking_adaptive)
            add("thinking adaptive" + (m.default_effort.empty() ? "" : " " + m.default_effort));
        else if (m.thinking_legacy)
            add("thinking budget");
        else if (m.reasoning)
            add("reasoning");
        if (std::find(m.input_modalities.begin(), m.input_modalities.end(), "image") !=
            m.input_modalities.end())
            add("image");
        if (!help.empty())
            leaf["help"] = help;
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
        pm.max_output = m.max_output;
        pm.reasoning = m.reasoning;
        pm.thinking_adaptive = m.thinking_adaptive;
        pm.thinking_legacy = m.thinking_legacy;
        pm.thinking_always_on = m.thinking_always_on;
        pm.default_effort = m.default_effort;
        pm.input_modalities = m.input_modalities;
        out.push_back(std::move(pm));
    }
    return out;
}

namespace {

// Why the checkbox reads what it reads. The text says what to do about it, so a
// blank box is never mistaken for a broken provider and a bang never for a
// missing key.
std::string provider_reason(const ProviderListRow& row, bool has_endpoint, bool configured) {
    if (row.pending)
        return "checking ...";
    if (!has_endpoint)
        return "unconfigured";
    if (!configured)
        return "no API key";
    if (row.state == agent::AuthState::Rejected)
        return "token rejected";
    if (row.state == agent::AuthState::Unknown)
        return "no answer";
    return row.api_base;
}

} // namespace

rich::Line provider_list_line(const ProviderListRow& row, const std::string& ts) {
    const bool has_endpoint = !row.api_base.empty();
    // "Configured" is the question the checkbox answers: an endpoint exists and,
    // when one is required, a key is present. Anything less is "no config found"
    // -- the text says which half is missing.
    const bool configured = has_endpoint && (!row.requires_key || row.has_key);

    std::string text = "  [";
    text += row.pending ? ' ' : agent::provider_mark(row.state, configured);
    text += "]  ";
    text += row.name;
    text += "  (";
    text += provider_reason(row, has_endpoint, configured);
    text += ")";

    rich::Line line;
    if (!ts.empty())
        line.runs.push_back({ts, P_REASONING, false, true}); // faint timestamp
    rich::Run body;
    body.pair = (row.active && !row.pending) ? P_ACTIVE : P_STATUS;
    body.dim = row.pending;
    body.text = std::move(text);
    line.runs.push_back(std::move(body));
    return line;
}

void PendingProviderRows::add(size_t window_id, size_t index, unsigned request, std::string ts,
                              ProviderListRow row) {
    entries_.push_back(Entry{window_id, index, request, std::move(ts), std::move(row)});
}

std::optional<PendingProviderRows::Settled>
PendingProviderRows::settle(unsigned request, const std::string& name, agent::AuthState state) {
    auto it = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) {
        return e.request == request && e.row.name == name;
    });
    if (it == entries_.end())
        return std::nullopt;
    ProviderListRow row = it->row;
    row.pending = false;
    row.state = state;
    Settled out{it->window_id, it->index, provider_list_line(row, it->ts)};
    entries_.erase(it);
    return out;
}

void PendingProviderRows::trimmed(size_t window_id, size_t removed) {
    for (auto& e : entries_) {
        if (e.window_id != window_id)
            continue;
        if (e.index < removed)
            e.index = std::string::npos;
        else
            e.index -= removed;
    }
    // A trimmed-away row has nothing left to rewrite: drop it rather than keep
    // an index that can never be valid.
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [](const Entry& e) { return e.index == std::string::npos; }),
                   entries_.end());
}

void PendingProviderRows::drop_window(size_t window_id) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&](const Entry& e) { return e.window_id == window_id; }),
                   entries_.end());
}

} // namespace tui