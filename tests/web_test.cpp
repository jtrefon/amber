#include "agent/config.h"
#include "agent/event_bus.h"
#include "agent/plugin_core.h"
#include "agent/plugin_runtime.h"
#include "agent/registry.h"
#include "agent/tool.h"
#include "agent/workspace.h"
#include "plugins/web/web_plugin.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "tests/test_util.h"

namespace {

namespace fs = std::filesystem;

class FakeTransport : public agent::plugins::web::WebTransport {
public:
    agent::plugins::web::HttpResponse response{200, ""};
    std::string requested;

    std::optional<agent::plugins::web::HttpResponse> get(const std::string& url, int,
                                                         int) override {
        requested = url;
        return response;
    }
};

class DummyTool : public agent::Tool {
public:
    explicit DummyTool(std::string name) : name_(std::move(name)) {}
    std::string name() const noexcept override { return name_; }
    std::string description() const noexcept override { return "dummy"; }
    agent::json parameters_schema() const override { return agent::json::object(); }
    agent::ToolResult execute(const agent::json&) const override { return {}; }

private:
    std::string name_;
};

class ConfigGuard {
public:
    explicit ConfigGuard(const std::string& dir) {
        const char* old = std::getenv("XDG_CONFIG_HOME");
        set_ = old != nullptr;
        if (set_)
            old_ = old;
        setenv("XDG_CONFIG_HOME", dir.c_str(), 1);
    }
    ~ConfigGuard() {
        if (set_)
            setenv("XDG_CONFIG_HOME", old_.c_str(), 1);
        else
            unsetenv("XDG_CONFIG_HOME");
    }

private:
    bool set_ = false;
    std::string old_;
};

} // namespace

TEST(web_parse_extracts_text_and_strips_scripts) {
    const std::string html =
        "<html><style>.x{}</style><script>alert(1)</script><h1>Title &amp; more</h1>"
        "<p>Hello <b>world</b>.</p></html>";
    ASSERT_EQ(agent::plugins::web::html_to_text(html), "Title & more\nHello world.");
    ASSERT_EQ(agent::plugins::web::decode_entities("&lt;&#65;&gt;"), "<A>");
}

TEST(web_parse_rejects_unsafe_urls) {
    std::string error;
    ASSERT_TRUE(agent::plugins::web::safe_web_url("https://example.com/a?b=c", error));
    ASSERT_FALSE(agent::plugins::web::safe_web_url("file:///etc/passwd", error));
    ASSERT_FALSE(agent::plugins::web::safe_web_url("https://user:pass@example.com", error));
    ASSERT_FALSE(agent::plugins::web::safe_web_url("https://example.com/a b", error));
}

TEST(web_parse_search_results) {
    const std::string html =
        R"(<a class="result__a" href="//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.com%2Fa">One &amp; more</a>)"
        R"(<a class="result__snippet">First snippet</a>)"
        R"(<a class="result__a" href="https://example.com/b">Two</a>)"
        R"(<a class="result__snippet">Second snippet</a>)";
    const auto results = agent::plugins::web::parse_search_results(html);
    ASSERT_EQ(results.size(), 2u);
    ASSERT_EQ(results[0].title, "One & more");
    ASSERT_EQ(results[0].url, "https://example.com/a");
    ASSERT_EQ(results[0].snippet, "First snippet");
    ASSERT_EQ(results[1].url, "https://example.com/b");
}

TEST(web_client_error_paths_and_caps) {
    auto transport = std::make_shared<FakeTransport>();
    agent::plugins::web::WebSettings settings;
    agent::plugins::web::WebClient client(settings, transport);
    std::string error;

    ASSERT_FALSE(client.fetch("file:///etc/passwd", error).has_value());
    transport->response = {500, "nope"};
    ASSERT_FALSE(client.fetch("https://example.com", error).has_value());
    transport->response = {200, "<script>only</script>"};
    ASSERT_FALSE(client.fetch("https://example.com", error).has_value());
    transport->response = {200, "<p>" + std::string(4000, 'a') + "</p>"};
    settings.max_bytes = 1024;
    agent::plugins::web::WebClient capped(settings, transport);
    const auto text = capped.fetch("https://example.com", error);
    ASSERT_TRUE(text.has_value());
    ASSERT(text->size() <= 1024u);

    settings.search_url = "https://example.com/search";
    agent::plugins::web::WebClient no_placeholder(settings, transport);
    transport->response = {
        200,
        R"(<a class="result__a" href="https://example.com/a">A</a><a class="result__snippet">S</a>)"};
    const auto results = no_placeholder.search("hello world", 1, error);
    ASSERT_EQ(results.size(), 1u);
    ASSERT(transport->requested.find("?q=hello%20world") != std::string::npos);
    ASSERT_FALSE(no_placeholder.search("", 1, error).size());
    transport->response = {503, ""};
    ASSERT_TRUE(no_placeholder.search("x", 1, error).empty());

    agent::plugins::web::WebClient missing(settings, nullptr);
    ASSERT_FALSE(missing.fetch("https://example.com", error).has_value());
    ASSERT_TRUE(missing.search("x", 1, error).empty());
}

TEST(web_curl_transport_handles_connection_failure) {
    agent::plugins::web::CurlWebTransport transport;
    ASSERT_FALSE(transport.get("http://127.0.0.1:1/", 1, 1024).has_value());
}

TEST(web_plugin_registers_settings_and_tools) {
    const std::string base = "/tmp/amber_web_runtime_test";
    fs::remove_all(base);
    fs::create_directories(base);
    ConfigGuard guard(base);

    agent::ToolRegistry tools;
    agent::Config config;
    agent::Workspace workspace;
    agent::PluginRuntime runtime(tools, config, workspace);
    runtime.add_bundled();
    runtime.start(false);
    auto fetch = tools.find("web_fetch");
    auto search = tools.find("web_search");
    ASSERT_TRUE(fetch != nullptr);
    ASSERT_TRUE(search != nullptr);
    ASSERT(fetch->is_read_only());
    ASSERT(fetch->parameters_schema()["properties"].contains("url"));
    ASSERT(search->parameters_schema()["properties"].contains("query"));
    ASSERT_FALSE(fetch->execute({{"url", "file:///etc/passwd"}}).ok);
    ASSERT_FALSE(search->execute({{"query", ""}}).ok);

    const auto* provider = runtime.settings().find("web", "provider");
    ASSERT_TRUE(provider != nullptr);
    ASSERT_EQ(runtime.settings().get("web", "provider"), std::string("builtin"));
    ASSERT_FALSE(runtime.settings().set("web", "provider", "bogus"));
    ASSERT_TRUE(runtime.settings().set("web", "provider", "off"));
    ASSERT_FALSE(fetch->execute({{"url", "https://example.com"}}).ok);
    ASSERT_TRUE(runtime.settings().set("web", "provider", "builtin"));
    ASSERT_TRUE(runtime.settings().set("web", "timeout_s", "5"));
    ASSERT_FALSE(runtime.settings().set("web", "timeout_s", "0"));
    ASSERT_TRUE(runtime.settings().set("web", "max_bytes", "2048"));
    ASSERT_FALSE(runtime.settings().set("web", "max_bytes", "1"));
    ASSERT_TRUE(runtime.settings().set("web", "search_url", "https://example.com/?q={query}"));
    ASSERT_FALSE(runtime.settings().set("web", "search_url", "file:///x"));
    fs::remove_all(base);
}

TEST(web_plugin_loads_persisted_settings) {
    const std::string base = "/tmp/amber_web_settings_test";
    fs::remove_all(base);
    fs::create_directories(base + "/amber/plugins/web");
    {
        std::ofstream out(base + "/amber/plugins/web/web.conf", std::ios::trunc);
        out << "provider=off\n"
            << "timeout_s=7\n"
            << "max_bytes=4096\n"
            << "search_url=https://example.com/search?q={query}\n";
    }
    ConfigGuard guard(base);
    agent::plugins::WebPlugin plugin(std::make_shared<FakeTransport>());
    agent::EventBus bus;
    agent::ToolRegistry tools;
    agent::Config config;
    agent::Workspace workspace;
    ASSERT_TRUE(plugin.initialize(agent::PluginContext{bus, tools, &config, workspace}));
    ASSERT_EQ(plugin.settings().provider, "off");
    ASSERT_EQ(plugin.settings().timeout_s, 7);
    ASSERT_EQ(plugin.settings().max_bytes, 4096);
    ASSERT(plugin.settings().search_url.find("{query}") != std::string::npos);
    fs::remove_all(base);
}

TEST(web_plugin_provider_conflict_and_fetch) {
    const std::string base = "/tmp/amber_web_test";
    fs::remove_all(base);
    fs::create_directories(base);
    ConfigGuard guard(base);

    auto transport = std::make_shared<FakeTransport>();
    transport->response = {200, "<p>hello <b>web</b></p>"};
    agent::plugins::WebPlugin plugin(transport);
    agent::EventBus bus;
    agent::ToolRegistry tools;
    agent::Config config;
    agent::Workspace workspace;
    agent::PluginContext context{bus, tools, &config, workspace};
    ASSERT_TRUE(plugin.initialize(context));

    ASSERT_TRUE(plugin.set_provider("off"));
    auto off = plugin.fetch({{"url", "https://example.com"}});
    ASSERT_FALSE(off.ok);
    ASSERT(off.error.find("off") != std::string::npos);

    ASSERT_FALSE(plugin.set_provider("cdp")); // no cdp tool registered
    tools.register_tool(std::make_unique<DummyTool>("plugin_cdp_navigate"), "cdp");
    ASSERT_TRUE(plugin.set_provider("cdp"));
    auto cdp = plugin.fetch({{"url", "https://example.com"}});
    ASSERT_FALSE(cdp.ok);
    ASSERT(cdp.error.find("cdp") != std::string::npos);

    ASSERT_TRUE(plugin.set_provider("builtin"));
    auto fetched = plugin.fetch({{"url", "https://example.com"}});
    ASSERT_TRUE(fetched.ok);
    ASSERT_EQ(fetched.output, "hello web");

    transport->response = {
        200,
        R"(<a class="result__a" href="https://example.com">Example</a><a class="result__snippet">Snippet</a>)"};
    auto searched = plugin.search({{"query", "example"}, {"max_results", 3}});
    ASSERT_TRUE(searched.ok);
    ASSERT(searched.output.find("https://example.com") != std::string::npos);
    ASSERT(transport->requested.find("q=example") != std::string::npos);

    fs::remove_all(base);
}
