#include "agent/provider_health.h"
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
// cache file: a provider with no api_base has no /models to read, and asking it
// for one would be the bug.
//
// Credentials are deliberately not part of it. Listing reads a cache that is
// already on disk, and gating the read on a key emptied the drawer for exactly
// the providers the user had configured. Whether a FETCH is worth attempting is
// the caller's call, and it lives with the caller -- refresh_provider_catalogs_async()
// is where builtin/requires_key are known.
namespace {

std::vector<tui::ProviderModel> counting_reader(const tui::ProviderEndpoint& e) {
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

// The regression, pinned at this seam: a provider whose key is missing is still
// listed from its cache. "No key" is a reason not to FETCH -- a decision the
// fetch path makes for itself, where builtin/requires_key are known -- never a
// reason to hide a catalogue that is already on disk. Gating the read on key
// availability is how /set model went empty.
TEST(model_picker_a_keyless_endpoint_is_still_listable) {
    std::vector<tui::ProviderEndpoint> endpoints{
        {"openrouter", "https://openrouter.ai/api/v1", "openai"}, // configured, no key
        {"kilocode", "https://api.kilo.ai/api/gateway", "openai"},
    };
    auto catalogs = tui::catalogs_from(endpoints, &counting_reader);
    ASSERT_EQ(catalogs.size(), 2u);
    ASSERT_TRUE(catalogs[0].configured);
    ASSERT_EQ(catalogs[0].models.size(), 1u); // read, not skipped
    ASSERT_TRUE(catalogs[1].configured);
    ASSERT_EQ(catalogs[1].models.size(), 1u);
    // The drawer's aggregate carries both providers, the active one leading.
    auto rows = tui::aggregate_provider_models(catalogs, "kilocode");
    ASSERT_EQ(rows.size(), 2u);
    ASSERT_EQ(rows[0].provider, std::string("kilocode"));
    ASSERT_EQ(rows[1].provider, std::string("openrouter"));
}

// /get provider list has to answer "will this provider work?" from an answer
// that was just fetched, and it answers for every provider at once: the list is
// printed immediately -- one dim row per provider -- and each row is rewritten
// in place when its own probe lands.

namespace {

tui::ProviderListRow row_for(const std::string& name, const std::string& api_base) {
    tui::ProviderListRow row;
    row.name = name;
    row.api_base = api_base;
    return row;
}

const tui::rich::Run& body_of(const tui::rich::Line& line) {
    return line.runs.back();
}

} // namespace

TEST(provider_list_row_marks_a_verified_provider) {
    auto row = row_for("kilocode", "https://api.kilo.ai/api/gateway");
    row.has_key = true;
    row.state = agent::AuthState::Valid;
    const auto line = tui::provider_list_line(row, "[00:00:00] ");
    ASSERT(body_of(line).text.find("[x]") != std::string::npos);
    ASSERT(body_of(line).text.find("https://api.kilo.ai/api/gateway") != std::string::npos);
    ASSERT_FALSE(body_of(line).dim);
}

TEST(provider_list_row_marks_a_rejected_token) {
    // Configured, keyed, and refused: [!] plus the reason, so it is not mistaken
    // for a provider that simply has no key yet.
    auto row = row_for("kilocode", "https://api.kilo.ai/api/gateway");
    row.has_key = true;
    row.state = agent::AuthState::Rejected;
    const auto line = tui::provider_list_line(row, "");
    ASSERT(body_of(line).text.find("[!]") != std::string::npos);
    ASSERT(body_of(line).text.find("token rejected") != std::string::npos);
}

TEST(provider_list_row_marks_an_unanswered_probe) {
    // The probe could not tell (timeout, refused, 5xx): [!] and no answer, never
    // [x] -- a credential nothing confirmed must not show a tick.
    auto row = row_for("openrouter", "https://openrouter.ai/api/v1");
    row.has_key = true;
    row.state = agent::AuthState::Unknown;
    const auto line = tui::provider_list_line(row, "");
    ASSERT(body_of(line).text.find("[!]") != std::string::npos);
    ASSERT(body_of(line).text.find("no answer") != std::string::npos);
}

TEST(provider_list_row_marks_an_unconfigured_provider) {
    const auto line = tui::provider_list_line(row_for("gemini", ""), "");
    ASSERT(body_of(line).text.find("[ ]") != std::string::npos);
    ASSERT(body_of(line).text.find("unconfigured") != std::string::npos);
}

TEST(provider_list_row_marks_a_missing_key) {
    // Endpoint present, a key required and absent: not configured, so an empty
    // box -- and the text says which half is missing.
    auto row = row_for("anthropic", "https://api.anthropic.com");
    row.requires_key = true;
    const auto line = tui::provider_list_line(row, "");
    ASSERT(body_of(line).text.find("[ ]") != std::string::npos);
    ASSERT(body_of(line).text.find("no API key") != std::string::npos);
}

TEST(provider_list_row_is_pending_until_the_probe_answers) {
    // Printed at once, dim, with an empty box: the answer has not arrived, so the
    // row must not claim anything.
    auto row = row_for("kilocode", "https://api.kilo.ai/api/gateway");
    row.has_key = true;
    row.pending = true;
    const auto line = tui::provider_list_line(row, "[00:00:00] ");
    ASSERT(body_of(line).text.find("[ ]") != std::string::npos);
    ASSERT(body_of(line).text.find("checking") != std::string::npos);
    ASSERT_TRUE(body_of(line).dim);
}

TEST(provider_list_row_colours_the_active_provider_green) {
    // The asterisk column is gone: the active provider is named by colour, so
    // every row starts in the same column.
    auto active = row_for("kilocode", "https://api.kilo.ai/api/gateway");
    active.has_key = true;
    active.active = true;
    active.state = agent::AuthState::Valid;
    const auto line = tui::provider_list_line(active, "");
    ASSERT_EQ(body_of(line).pair, static_cast<int>(tui::P_ACTIVE));
    ASSERT(body_of(line).text.find('*') == std::string::npos);

    auto other = row_for("openrouter", "https://openrouter.ai/api/v1");
    other.has_key = true;
    other.state = agent::AuthState::Valid;
    ASSERT_EQ(body_of(tui::provider_list_line(other, "")).pair, static_cast<int>(tui::P_STATUS));
}

TEST(provider_list_row_keeps_every_row_in_the_same_column) {
    // Whatever the state, the checkbox sits at the same column, so the list can
    // be read down; no asterisk shifts one row out of line.
    auto pending = row_for("a", "https://a.example/v1");
    pending.has_key = true;
    pending.pending = true;
    const auto unconfigured = row_for("bb", "");
    auto rejected = row_for("ccc", "https://ccc.example/v1");
    rejected.has_key = true;
    rejected.state = agent::AuthState::Rejected;
    for (const auto& row : {pending, unconfigured, rejected}) {
        const auto text = body_of(tui::provider_list_line(row, "")).text;
        ASSERT_EQ(text.substr(0, 3), std::string("  ["));
        ASSERT_EQ(text[4], ']');
    }
}

TEST(provider_list_row_carries_the_timestamp) {
    auto row = row_for("kilocode", "https://api.kilo.ai/api/gateway");
    row.has_key = true;
    row.state = agent::AuthState::Valid;
    const auto line = tui::provider_list_line(row, "[12:34:56] ");
    ASSERT_EQ(line.runs.size(), 2u);
    ASSERT_EQ(line.runs[0].text, std::string("[12:34:56] "));
    ASSERT_TRUE(line.runs[0].dim);
}

// The in-place settle: an answer must rewrite ITS row, and must survive the two
// things that move scrollback positions.

TEST(pending_provider_rows_settles_the_row_its_request_printed) {
    tui::PendingProviderRows rows;
    tui::ProviderListRow first = row_for("kilocode", "https://a.example/v1");
    first.has_key = true;
    first.pending = true;
    rows.add(7, 100, /*request=*/1, "[00:00:00] ", first);
    tui::ProviderListRow second = first;
    second.name = "openrouter";
    rows.add(7, 101, 2, "[00:00:00] ", second);

    auto settled = rows.settle(1, "kilocode", agent::AuthState::Valid);
    ASSERT(settled.has_value());
    ASSERT_EQ(settled->window_id, 7u);
    ASSERT_EQ(settled->index, 100u);
    ASSERT(settled->line.runs.back().text.find("[x]") != std::string::npos);
    ASSERT_EQ(rows.size(), 1u);
    // Another invocation's row is untouched: only its own answer settles it.
    ASSERT_FALSE(rows.settle(1, "openrouter", agent::AuthState::Valid).has_value());
    ASSERT_TRUE(rows.settle(2, "openrouter", agent::AuthState::Rejected).has_value());
}

TEST(pending_provider_rows_shift_with_a_trim) {
    tui::PendingProviderRows rows;
    const tui::ProviderListRow row = row_for("kilocode", "https://a.example/v1");
    rows.add(7, 4999, 1, "", row); // trimmed away with the oldest scrollback
    rows.add(7, 5000, 1, "", row); // survives, and moves to the front
    rows.trimmed(7, 5000);
    ASSERT_EQ(rows.size(), 1u);
    auto settled = rows.settle(1, "kilocode", agent::AuthState::Valid);
    ASSERT(settled.has_value());
    ASSERT_EQ(settled->index, 0u);
}

TEST(pending_provider_rows_drop_with_their_window) {
    // A session restore replaces the whole scrollback, so every index into it is
    // gone; writing to one would corrupt the restored session.
    tui::PendingProviderRows rows;
    rows.add(7, 100, 1, "", row_for("kilocode", "https://a.example/v1"));
    rows.drop_window(7);
    ASSERT_EQ(rows.size(), 0u);
    ASSERT_FALSE(rows.settle(1, "kilocode", agent::AuthState::Valid).has_value());
}

TEST(model_picker_empty_rows_produce_an_empty_subtree) {
    auto sub = tui::model_subtree({}, "a.");
    ASSERT_EQ(sub["set"]["children"]["model"]["children"].size(), 0u);
}

// The cache read is exercised for real rather than mocked: the catalogue is
// written under the XDG cache root ($XDG_CACHE_HOME/amber) keyed by
// api_base+flavor, so pointing the roots at a temporary directory and writing
// the file proves the whole path -- including that the FLAVOUR reaches the
// key. A provider that does not speak openai must not read openai's cache.
namespace {

struct ScopedConfigHome {
    std::string saved;
    std::string saved_cache;
    ScopedConfigHome() {
        const char* old = std::getenv("XDG_CONFIG_HOME");
        saved = old ? old : "";
        const char* old_cache = std::getenv("XDG_CACHE_HOME");
        saved_cache = old_cache ? old_cache : "";
        dir = std::filesystem::temp_directory_path() / "amber_picker_test_cfg";
        std::filesystem::create_directories(dir / "cache" / "amber");
        setenv("XDG_CONFIG_HOME", dir.c_str(), 1);
        setenv("XDG_CACHE_HOME", (dir / "cache").c_str(), 1);
    }
    ~ScopedConfigHome() {
        if (saved.empty())
            unsetenv("XDG_CONFIG_HOME");
        else
            setenv("XDG_CONFIG_HOME", saved.c_str(), 1);
        if (saved_cache.empty())
            unsetenv("XDG_CACHE_HOME");
        else
            setenv("XDG_CACHE_HOME", saved_cache.c_str(), 1);
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

void write_catalogue(const std::filesystem::path& cache_root, const std::string& api_base,
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
    std::ofstream f(cache_root / "cache" / "amber" / name);
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