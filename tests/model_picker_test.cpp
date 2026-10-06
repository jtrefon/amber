#include "tui/model_picker.h"
#include "tests/test_util.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace {

tui::ProviderCatalog cat(const std::string& provider, bool configured,
                         std::vector<std::string> ids) {
    tui::ProviderCatalog c;
    c.provider = provider;
    c.configured = configured;
    for (const auto& id : ids) {
        tui::ProviderModel m;
        m.provider = provider;
        m.id = id;
        m.context = 200000;
        c.models.push_back(m);
    }
    return c;
}

} // namespace

// A vendor-prefixed id and a provider name are different things, and the bar
// used to show only the id: "[kilo-auto/free(high)]" named no provider at all,
// because "kilo-auto" is the model's VENDOR segment.
TEST(model_picker_key_splits_provider_from_model) {
    std::string provider, id;
    ASSERT(tui::split_provider_key("kilocode::kilo-auto/free", provider, id));
    ASSERT_EQ(provider, std::string("kilocode"));
    ASSERT_EQ(id, std::string("kilo-auto/free"));
}

TEST(model_picker_key_round_trips) {
    std::string provider, id;
    ASSERT(tui::split_provider_key(tui::provider_key("openrouter", "anthropic/claude-opus-4.8"),
                                   provider, id));
    ASSERT_EQ(provider, std::string("openrouter"));
    ASSERT_EQ(id, std::string("anthropic/claude-opus-4.8"));
}

// A bare id has no owner. It must still parse as "no provider", not as an
// empty provider with the whole id as the model.
TEST(model_picker_key_without_separator_has_no_provider) {
    std::string provider = "untouched", id = "untouched";
    ASSERT_FALSE(tui::split_provider_key("kilo-auto/free", provider, id));
    ASSERT_EQ(provider, std::string("untouched"));
}

// Two providers offering one id must not collide on the tree key, which is why
// the key is composite.
TEST(model_picker_two_providers_offering_one_id_get_distinct_keys) {
    const std::string a = tui::provider_key("kilocode", "anthropic/claude-opus-4.8");
    const std::string b = tui::provider_key("openrouter", "anthropic/claude-opus-4.8");
    ASSERT(a != b);
    std::string pa, pb, ia, ib;
    ASSERT(tui::split_provider_key(a, pa, ia));
    ASSERT(tui::split_provider_key(b, pb, ib));
    ASSERT(pa != pb);
    ASSERT_EQ(ia, ib);
}

// The whole point: models the active provider does not carry must appear.
TEST(model_picker_aggregates_every_configured_provider) {
    std::vector<tui::ProviderCatalog> catalogs{
        cat("kilocode", true, {"kilo-auto/free", "kilo-auto/frontier"}),
        cat("openrouter", true, {"anthropic/claude-opus-4.8", "qwen/qwen3"}),
    };
    auto rows = tui::aggregate_provider_models(catalogs, "kilocode");
    ASSERT_EQ(rows.size(), 4u);
    bool saw_foreign = false;
    for (const auto& r : rows)
        if (r.provider == "openrouter")
            saw_foreign = true;
    ASSERT(saw_foreign);
}

// The active provider's block leads, so the list a user was already looking at
// stays where it was instead of jumping. The rest keep the order given, so the
// result is deterministic rather than dependent on registry iteration order.
TEST(model_picker_active_provider_block_leads) {
    std::vector<tui::ProviderCatalog> catalogs{
        cat("openrouter", true, {"a/1"}),
        cat("kilocode", true, {"b/1"}),
        cat("gemini", true, {"c/1"}),
    };
    auto rows = tui::aggregate_provider_models(catalogs, "gemini");
    ASSERT_EQ(rows.size(), 3u);
    ASSERT_EQ(rows[0].provider, std::string("gemini"));
    ASSERT_EQ(rows[1].provider, std::string("openrouter"));
    ASSERT_EQ(rows[2].provider, std::string("kilocode"));
}

// Whichever provider is active, its rows come first -- including when it is
// already first, which must not rotate the list.
TEST(model_picker_active_provider_leads_when_already_first) {
    std::vector<tui::ProviderCatalog> catalogs{
        cat("kilocode", true, {"a/1"}),
        cat("openrouter", true, {"b/1"}),
    };
    auto rows = tui::aggregate_provider_models(catalogs, "kilocode");
    ASSERT_EQ(rows.size(), 2u);
    ASSERT_EQ(rows[0].provider, std::string("kilocode"));
    ASSERT_EQ(rows[1].provider, std::string("openrouter"));
}

// An active provider that is not configured contributes nothing, and must not
// push the others around or crash on the lookup.
TEST(model_picker_active_provider_absent_from_catalogs_is_fine) {
    std::vector<tui::ProviderCatalog> catalogs{
        cat("kilocode", true, {"a/1"}),
        cat("openrouter", true, {"b/1"}),
    };
    auto rows = tui::aggregate_provider_models(catalogs, "anthropic");
    ASSERT_EQ(rows.size(), 2u);
    ASSERT_EQ(rows[0].provider, std::string("kilocode"));
}

// Every copy is listed separately: a row says which provider picking it would
// switch TO, so collapsing duplicates would hide that choice.
TEST(model_picker_keeps_every_providers_copy_of_a_shared_id) {
    std::vector<tui::ProviderCatalog> catalogs{
        cat("kilocode", true, {"anthropic/claude-opus-4.8"}),
        cat("openrouter", true, {"anthropic/claude-opus-4.8"}),
    };
    auto rows = tui::aggregate_provider_models(catalogs, "kilocode");
    ASSERT_EQ(rows.size(), 2u);
    ASSERT_EQ(rows[0].provider, std::string("kilocode"));
    ASSERT_EQ(rows[1].provider, std::string("openrouter"));
    ASSERT_EQ(rows[0].id, rows[1].id);
}

// An unconfigured provider has no /models to fetch, and an empty catalogue has
// nothing to offer; a tab for either would always read zero.
TEST(model_picker_skips_unconfigured_and_empty_providers) {
    std::vector<tui::ProviderCatalog> catalogs{
        cat("kilocode", true, {"a/1"}),
        cat("openrouter", false, {"b/1", "b/2"}),
        cat("anthropic", true, {}),
    };
    auto rows = tui::aggregate_provider_models(catalogs, "kilocode");
    ASSERT_EQ(rows.size(), 1u);
    ASSERT_EQ(rows.front().provider, std::string("kilocode"));
}

TEST(model_picker_empty_input_yields_no_rows) {
    ASSERT_EQ(tui::aggregate_provider_models({}, "kilocode").size(), 0u);
}

// The reader is injected so the "has an endpoint" rule is exercised without a
// cache file: a provider with no api_base has no /models, and asking it for one
// would be the bug.
namespace {

bool reader_was_called = false;
std::vector<tui::ProviderModel> counting_reader(const tui::ProviderEndpoint& e) {
    reader_was_called = true;
    tui::ProviderModel m;
    m.provider = e.name;
    m.id = e.name + "/only";
    return {m};
}

} // namespace

TEST(model_picker_catalogs_skip_endpoints_with_no_api_base) {
    std::vector<tui::ProviderEndpoint> endpoints{
        {"kilocode", "https://api.kilo.ai/api/gateway", "openai"},
        {"openrouter", "", "openai"},
        {"anthropic", "https://api.anthropic.com/v1", "anthropic"},
    };
    auto catalogs = tui::catalogs_from(endpoints, &counting_reader);
    ASSERT_EQ(catalogs.size(), 3u);
    ASSERT_TRUE(catalogs[0].configured);
    ASSERT_EQ(catalogs[0].models.size(), 1u);
    ASSERT_FALSE(catalogs[1].configured);
    ASSERT_EQ(catalogs[1].models.size(), 0u); // never read
    ASSERT_TRUE(catalogs[2].configured);
    // The flavour is carried through: a provider that does not speak openai
    // must be parsed with its own dialect, so the cache key must match.
    ASSERT_EQ(catalogs[2].provider, std::string("anthropic"));
}

TEST(model_picker_leaf_keys_are_composite_and_carry_action_and_ctx) {
    std::vector<tui::ProviderModel> rows{
        {"kilocode", "kilo-auto/free", 256000},
        {"openrouter", "anthropic/claude-opus-4.8", 200000},
        {"anthropic", "no-ctx-model", 0},
    };
    auto sub = tui::model_subtree(rows, "core.config.set.model.");
    const auto& leaves = sub["set"]["children"]["model"]["children"];
    ASSERT_EQ(leaves.size(), 3u);
    ASSERT(leaves.contains("kilocode::kilo-auto/free"));
    ASSERT(leaves.contains("openrouter::anthropic/claude-opus-4.8"));
    ASSERT_EQ(leaves["kilocode::kilo-auto/free"]["action"].get<std::string>(),
              std::string("core.config.set.model.kilocode::kilo-auto/free"));
    ASSERT_EQ(leaves["kilocode::kilo-auto/free"]["help"].get<std::string>(),
              std::string("ctx 256000"));
    // Unknown context must not render a bogus "ctx 0".
    ASSERT_FALSE(leaves["anthropic::no-ctx-model"].contains("help"));
}

// The whole reason keys are composite: two providers, one id, two leaves.
TEST(model_picker_subtree_keeps_both_providers_of_a_shared_id) {
    std::vector<tui::ProviderModel> rows{
        {"kilocode", "anthropic/claude-opus-4.8", 200000},
        {"openrouter", "anthropic/claude-opus-4.8", 200000},
    };
    auto sub = tui::model_subtree(rows, "a.");
    const auto& leaves = sub["set"]["children"]["model"]["children"];
    ASSERT_EQ(leaves.size(), 2u);
    ASSERT(leaves.contains("kilocode::anthropic/claude-opus-4.8"));
    ASSERT(leaves.contains("openrouter::anthropic/claude-opus-4.8"));
}

TEST(model_picker_empty_rows_produce_an_empty_subtree) {
    auto sub = tui::model_subtree({}, "a.");
    ASSERT_EQ(sub["set"]["children"]["model"]["children"].size(), 0u);
}

// The cache read is exercised for real rather than mocked: the catalogue is
// written under $XDG_CONFIG_HOME/amber/cache keyed by api_base+flavor, so
// pointing XDG_CONFIG_HOME at a temporary directory and writing the file
// proves the whole path -- including that the FLAVOUR reaches the key. A
// provider that does not speak openai must not read openai's cache.
namespace {

struct ScopedConfigHome {
    std::string saved;
    ScopedConfigHome() {
        const char* old = std::getenv("XDG_CONFIG_HOME");
        saved = old ? old : "";
        dir = std::filesystem::temp_directory_path() / "amber_picker_test_cfg";
        std::filesystem::create_directories(dir / "amber" / "cache");
        setenv("XDG_CONFIG_HOME", dir.c_str(), 1);
    }
    ~ScopedConfigHome() {
        if (saved.empty())
            unsetenv("XDG_CONFIG_HOME");
        else
            setenv("XDG_CONFIG_HOME", saved.c_str(), 1);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    std::filesystem::path dir;
};

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::size_t fnv1a(const std::string& key) {
    std::size_t h = 1469598103934665603ULL;
    for (unsigned char c : key) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

void write_catalogue(const std::filesystem::path& cfg_root, const std::string& api_base,
                     const std::string& flavor, const std::string& body) {
    char name[64];
    std::snprintf(name, sizeof(name), "models-%016zx.json", fnv1a(api_base + "\n" + flavor));
    // The catalogue cache stores the response body as a JSON *string*, not as
    // an embedded object -- the file on disk is {"body":"{\"data\":[...]}"} --
    // so a test that writes an object here silently exercises nothing.
    std::string quoted;
    for (const char c : body) {
        if (c == '"' || c == '\\')
            quoted += '\\';
        quoted += c;
    }
    std::ofstream f(cfg_root / "amber" / "cache" / name);
    f << R"({"fetched_ms":)" << now_ms() << R"(,"body":")" << quoted << R"("})";
}

} // namespace

TEST(model_picker_reads_a_providers_cached_catalogue) {
    ScopedConfigHome home;
    write_catalogue(home.dir, "https://api.kilo.ai/api/gateway", "openai",
                    R"({"data":[{"id":"kilo-auto/free","context_length":256000},)"
                    R"({"id":"kilo-auto/frontier"}]})");
    tui::ProviderEndpoint e;
    e.name = "kilocode";
    e.api_base = "https://api.kilo.ai/api/gateway";
    auto rows = tui::cached_models_for(e);
    ASSERT_EQ(rows.size(), 2u);
    ASSERT_EQ(rows[0].provider, std::string("kilocode"));
    ASSERT_EQ(rows[0].id, std::string("kilo-auto/free"));
    ASSERT(rows[0].context > 0);
}

// The cache key is api_base+flavor, so a provider speaking a different dialect
// must not pick up another provider's listing.
TEST(model_picker_cache_read_is_keyed_by_flavor_too) {
    ScopedConfigHome home;
    write_catalogue(home.dir, "https://api.anthropic.com/v1", "anthropic",
                    R"({"data":[{"id":"claude-opus-4.8"}]})");
    tui::ProviderEndpoint wrong;
    wrong.name = "anthropic";
    wrong.api_base = "https://api.anthropic.com/v1";
    wrong.flavor = "openai"; // same endpoint, different dialect
    ASSERT_EQ(tui::cached_models_for(wrong).size(), 0u);
    tui::ProviderEndpoint right = wrong;
    right.flavor = "anthropic";
    ASSERT_EQ(tui::cached_models_for(right).size(), 1u);
}

// A cold or absent cache must yield nothing rather than throwing or inventing a
// listing: the feed treats empty as "this provider has nothing yet".
TEST(model_picker_missing_catalogue_yields_no_rows) {
    ScopedConfigHome home;
    tui::ProviderEndpoint e;
    e.name = "openrouter";
    e.api_base = "https://openrouter.ai/api/v1";
    ASSERT_EQ(tui::cached_models_for(e).size(), 0u);
}