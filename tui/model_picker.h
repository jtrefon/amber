#pragma once

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

// One provider's endpoint, as the picker needs it. Decoupled from Provider so the
// gathering below can be tested without a repository stack or a cache on disk.
struct ProviderEndpoint {
    std::string name;
    std::string api_base;
    std::string flavor = "openai";
    // Whether this endpoint can actually be queried: it has one, and if the
    // provider needs a key then the key is present.
    //
    // api_base alone is NOT enough. Every bundled provider ships a preset
    // api_base (plugins/openrouter sets one), so "has an endpoint" is true for
    // all of them whether or not they are configured or hold a key -- and
    // treating that as "configured" fans a request out to every bundled
    // endpoint at once, unauthenticated.
    bool usable = false;
};

// Reads one endpoint's already-cached models. Must not touch the network: this
// runs while the UI thread is composing a drawer.
using CatalogReader = std::vector<ProviderModel> (*)(const ProviderEndpoint&);

// Per-provider catalogues, skipping endpoints that cannot be fetched.
//
// Split out from the feed so the "is it configured" rule is testable: a
// provider with an empty api_base has no catalogue, and an empty catalogue
// means there is nothing to list and no tab worth opening. Reading is injected
// so the rule is exercised without a cache file.
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

} // namespace tui