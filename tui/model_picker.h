#pragma once

#include <string>
#include <vector>

namespace tui {

// One row of the model picker: a model, and the provider that serves it.
//
// `/set model` used to list the ACTIVE provider's catalogue only, because
// list_model_info_cached() reads one cache file keyed by api_base+flavor. With
// several providers configured that hides every model the active provider does
// not carry, and nothing on screen said whose list was on display -- a
// vendor-prefixed id like "kilo-auto/free" reads as if "kilo-auto" were the
// provider when it is the model's vendor segment.
struct ProviderModel {
    std::string provider; // provider name, e.g. "kilocode"
    std::string id;       // model id as the provider spells it
    int context = 0;      // context window, 0 when unknown
};

// The separator between provider and model in a leaf key.
//
// "::" appears in neither a provider name nor a vendor-prefixed model id, so the
// split is unambiguous, and the composite stays typeable: "/set model
// kilocode::kilo-auto/free" works with no drawer at all. A composite key is
// required because the command tree is keyed by leaf name, and two providers
// offering one id would otherwise collide on it.
inline constexpr char kProviderSep[] = "::";

// Split "provider::id" back into its parts. Returns false when the key has no
// separator, so a bare model id (typed by hand, or from a tree written before
// this existed) still resolves -- it simply has no owner.
bool split_provider_key(const std::string& key, std::string& provider, std::string& id);

// The key for a (provider, model) pair.
std::string provider_key(const std::string& provider, const std::string& id);

// One provider's catalogue, in the order it will contribute rows.
struct ProviderCatalog {
    std::string provider;
    std::vector<ProviderModel> models; // this provider's models only
    bool configured = false;           // has an endpoint, so /models is fetchable
};

// Union of every configured provider's catalogue, active provider first, then
// the rest in the order given.
//
// Every provider's copy of a model is listed separately: if three providers
// carry "anthropic/claude-opus-4.8" the caller sees three rows, each labelled
// with its owner, because picking one switches provider and the row has to say
// which provider would be switched TO. Providers that are unconfigured or that
// contributed nothing are omitted -- they have no /models to offer, and a tab
// for them would always read zero.
std::vector<ProviderModel> aggregate_provider_models(const std::vector<ProviderCatalog>& catalogs,
                                                     const std::string& active_provider);

} // namespace tui