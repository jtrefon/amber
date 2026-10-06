#include "tui/model_picker.h"
#include "tests/test_util.h"

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