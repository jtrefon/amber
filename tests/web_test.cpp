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
    ASSERT_TRUE(tools.find("web_fetch") != nullptr);
    ASSERT_TRUE(tools.find("web_search") != nullptr);
    ASSERT_TRUE(runtime.settings().find("web", "provider") != nullptr);
    ASSERT_EQ(runtime.settings().get("web", "provider"), std::string("builtin"));
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
