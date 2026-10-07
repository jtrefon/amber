#pragma once

#include "agent/provider_health.h"

#include <nlohmann/json.hpp>

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
    bool configured = false;           // has an endpoint, so a catalogue could exist
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

// One provider's endpoint, as the picker needs it. Decoupled from Provider so the
// gathering below can be tested without a repository stack or a cache on disk.
struct ProviderEndpoint {
    std::string name;
    std::string api_base;
    std::string flavor = "openai";
};

// Reads one endpoint's already-cached models. Must not touch the network: this
// runs while the UI thread is composing a drawer.
using CatalogReader = std::vector<ProviderModel> (*)(const ProviderEndpoint&);

// Per-provider catalogues, skipping endpoints that have none.
//
// Split out from the feed so the rule is testable: a provider with an empty
// api_base has no catalogue, and an empty catalogue means there is nothing to
// list and no tab worth opening. "Has an endpoint" is read off the endpoint
// itself, not taken as a caller-set flag -- a flag every caller must remember
// is how the drawer emptied once, while every unit test stayed green. Reading
// is injected so the rule is exercised without a cache file.
std::vector<ProviderCatalog> catalogs_from(const std::vector<ProviderEndpoint>& endpoints,
                                           CatalogReader read);

// Read one endpoint's models from the on-disk catalogue cache.
//
// Lives here rather than in the feed so it is testable: the cache is keyed by
// api_base+flavor under $XDG_CONFIG_HOME/amber/cache, so a test can point
// XDG_CONFIG_HOME at a temporary directory, write a catalogue, and exercise the
// real read path -- including that the flavour reaches the key, since a provider
// that does not speak openai must not be parsed, or cached, as though it does.
//
// Cache-only by contract: this runs while the UI thread composes a drawer, so it
// must never fetch. A missing or unreadable cache yields nothing, which is the
// same as an empty catalogue.
std::vector<ProviderModel> cached_models_for(const ProviderEndpoint& endpoint);

// The command-tree subtree for a set of rows: one leaf per row, keyed by the
// composite so two providers offering one id cannot collide, carrying its action
// and (when known) its context window.
//
// `action` is a parameter because the action prefix belongs to whoever owns the
// feed; the leaf/collide/ctx rules are the same either way.
nlohmann::json model_subtree(const std::vector<ProviderModel>& rows,
                             const std::string& action_prefix);

// One line of /get provider list.
//
// The asterisk marks the ACTIVE provider, which it always did. The checkbox
// column answers the question the list used to leave open: is this provider set
// up at all? An enabled-but-unconfigured provider is indistinguishable from a
// configured one otherwise, which is why configuring openrouter appeared to do
// nothing.
//
//   [x]  has an endpoint, and a key if one is required
//   [!]  has an endpoint but no key -- it will fail to authenticate
//   [ ]  no endpoint, so there is nothing to query
//
// has_key is passed in rather than inferred: a bundled preset carries a preset
// api_base and no key, and the active config may hold the key from elsewhere, so
// One row of /get provider list.
//
// The checkbox reports what the SERVER last said about the credential, not
// whether a key happens to be present -- a revoked key and a working one were
// indistinguishable, which is the confusion this replaces:
//
//   [x]  configured, and the endpoint accepted the token
//   [!]  configured, but the token was rejected (or one is required and absent)
//   [ ]  not configured, or never probed -- never "probably fine"
//
// `state` is agent::AuthState. A provider that needs no key is Valid once the
// endpoint answers at all, which is what agent::provider_mark() decides.
std::string provider_list_line(const std::string& name, const std::string& api_base, bool active,
                               agent::AuthState state);

} // namespace tui