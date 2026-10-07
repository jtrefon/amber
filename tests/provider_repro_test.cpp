#include "tui/model_picker.h"

#include "agent/config.h"
#include "agent/model_probe.h"
#include "agent/providers.h"
#include "tests/test_util.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

// The /set model regression, reproduced end to end.
//
// The pure seams were tested -- aggregate_provider_models, catalogs_from,
// cached_models_for, model_subtree all have unit tests, and they pass. The empty
// drawer came from the WIRING between them and the real ProviderService: a
// provider whose api_base is not carried by available() was excluded from the
// list, and a list with no providers is an empty list, so /set model showed
// nothing at all.
//
// So this builds the service the same way the app does, through the public
// make_default_provider_service(), against a temporary config directory holding
// one user-configured provider and a matching catalogue cache, then asserts the
// provider is listable and its models reach the drawer subtree.

namespace {

struct ScopedConfigHome {
    std::string saved;
    std::filesystem::path dir;
    ScopedConfigHome() {
        const char* old = std::getenv("XDG_CONFIG_HOME");
        saved = old ? old : "";
        dir = std::filesystem::temp_directory_path() / "amber_provider_repro";
        std::filesystem::create_directories(dir / "amber" / "providers");
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
};

std::size_t fnv1a(const std::string& key) {
    std::size_t h = 1469598103934665603ULL;
    for (unsigned char c : key) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

void write_provider(const std::filesystem::path& root, const std::string& name,
                    const std::string& api_base, bool with_key) {
    std::ofstream f(root / "amber" / "providers" / (name + ".conf"));
    f << "provider=" << name << "\n";
    f << "api_base=" << api_base << "\n";
    if (with_key)
        f << "api_key=sk-test-not-a-real-key\n";
    f << "requires_key=1\n";
}

void write_catalogue(const std::filesystem::path& root, const std::string& api_base,
                     const std::string& flavor, const std::string& ids) {
    char name[64];
    std::snprintf(name, sizeof(name), "models-%016zx.json", fnv1a(api_base + "\n" + flavor));
    // The cache stores the body as a JSON STRING, so the inner document's quotes
    // must be escaped. Writing them raw produces invalid JSON and the reader
    // rejects the file -- which looks exactly like "no catalogue".
    std::string body = "{\"data\":[" + ids + "]}";
    std::string escaped;
    for (const char c : body) {
        if (c == '"' || c == '\\')
            escaped += '\\';
        escaped += c;
    }
    std::ofstream f(root / "amber" / "cache" / name);
    f << R"({"fetched_ms":)" << (long long)9'000'000'000'000ULL << R"(,"body":")" << escaped
      << R"("})";
}

// The endpoints the feed builds, from a real service.
std::vector<tui::ProviderEndpoint> endpoints_from(const agent::ProviderService& providers) {
    std::vector<tui::ProviderEndpoint> out;
    for (const auto& p : providers.available()) {
        tui::ProviderEndpoint e;
        e.name = p.name;
        e.api_base = p.api_base;
        e.flavor = p.flavor;
        e.has_endpoint = !p.api_base.empty();
        out.push_back(std::move(e));
    }
    return out;
}

} // namespace

// A provider the user configured has an endpoint, and therefore its catalogue is
// listable -- whatever its key state. This is the assertion that fails when the
// list is gated on credentials.
TEST(provider_repro_a_configured_provider_is_listable) {
    ScopedConfigHome home;
    write_provider(home.dir, "reproprov", "https://api.repro.example/v1", /*with_key=*/false);

    auto providers = agent::make_default_provider_service(agent::Config{});
    const auto endpoints = endpoints_from(*providers);

    bool found = false;
    for (const auto& e : endpoints)
        if (e.name == "reproprov")
            found = e.has_endpoint;
    ASSERT(found);
}

// And the whole path: cached catalogue -> aggregated rows -> drawer leaves.
// If any link in the wiring breaks, /set model is empty.
TEST(provider_repro_configured_provider_models_reach_the_drawer) {
    ScopedConfigHome home;
    write_provider(home.dir, "reproprov", "https://api.repro.example/v1", /*with_key=*/false);
    write_catalogue(home.dir, "https://api.repro.example/v1", "openai",
                    R"({"id":"repro/model-a","context_length":128000},)"
                    R"({"id":"repro/model-b"})");

    auto providers = agent::make_default_provider_service(agent::Config{});
    const auto endpoints = endpoints_from(*providers);
    const auto catalogs = tui::catalogs_from(endpoints, &tui::cached_models_for);
    const auto rows = tui::aggregate_provider_models(catalogs, "reproprov");

    ASSERT(!rows.empty());
    const auto subtree = tui::model_subtree(rows, "core.config.set.model.");
    const auto& leaves = subtree["set"]["children"]["model"]["children"];
    ASSERT(!leaves.empty());
    ASSERT(leaves.contains("reproprov::repro/model-a"));
}

// The same provider WITH a key must list identically: key state is not a
// precondition for reading a cache that is already on disk.
TEST(provider_repro_key_state_does_not_change_the_list) {
    ScopedConfigHome home;
    write_provider(home.dir, "reproprov", "https://api.repro.example/v1", /*with_key=*/true);
    write_catalogue(home.dir, "https://api.repro.example/v1", "openai",
                    R"({"id":"repro/model-a","context_length":128000})");

    auto providers = agent::make_default_provider_service(agent::Config{});
    const auto rows = tui::aggregate_provider_models(
        tui::catalogs_from(endpoints_from(*providers), &tui::cached_models_for), "reproprov");
    ASSERT(!rows.empty());
}

// The regression itself, stated as the contract: listing depends on having an
// endpoint, never on credentials. An endpoint with no key is still listable.
//
// (A bundled preset cannot be used for this: presets arrive through the plugin
// registry, which only the booted app populates, so in a unit test available()
// sees file providers only.)
TEST(provider_repro_listing_never_depends_on_credentials) {
    ScopedConfigHome home;
    write_provider(home.dir, "keyless", "https://api.keyless.example/v1", /*with_key=*/false);
    write_catalogue(home.dir, "https://api.keyless.example/v1", "openai",
                    R"({"id":"keyless/model","context_length":8000})");

    auto providers = agent::make_default_provider_service(agent::Config{});
    const auto rows = tui::aggregate_provider_models(
        tui::catalogs_from(endpoints_from(*providers), &tui::cached_models_for), "keyless");
    ASSERT_EQ(rows.size(), 1u);
    ASSERT_EQ(rows[0].provider, std::string("keyless"));
    ASSERT_EQ(rows[0].id, std::string("keyless/model"));
}
