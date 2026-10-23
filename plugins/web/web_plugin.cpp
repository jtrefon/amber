#include "web_plugin.h"

#include "agent/extensions.h"
#include "agent/prompt.h"
#include "agent/tool_prompt.h"
#include "agent/tools.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

namespace agent::plugins {

namespace {

class WebFetchTool : public Tool {
public:
    explicit WebFetchTool(WebPlugin& plugin) : plugin_(plugin) {}
    std::string name() const noexcept override { return "web_fetch"; }
    std::string description() const noexcept override {
        return "Fetch an http(s) URL and return its readable text.";
    }
    bool is_read_only() const noexcept override { return true; }
    json parameters_schema() const override {
        return json{
            {"type", "object"},
            {"properties",
             {{"url", {{"type", "string"}, {"description", "Absolute http(s) URL"}}},
              {"max_bytes", {{"type", "integer"}, {"description", "Optional response text cap"}}}}},
            {"required", json::array({"url"})}};
    }
    ToolResult execute(const json& arguments) const override { return plugin_.fetch(arguments); }

private:
    WebPlugin& plugin_;
};

class WebSearchTool : public Tool {
public:
    explicit WebSearchTool(WebPlugin& plugin) : plugin_(plugin) {}
    std::string name() const noexcept override { return "web_search"; }
    std::string description() const noexcept override {
        return "Search the web and return titles, URLs and snippets.";
    }
    bool is_read_only() const noexcept override { return true; }
    json parameters_schema() const override {
        return json{
            {"type", "object"},
            {"properties",
             {{"query", {{"type", "string"}, {"description", "Search query"}}},
              {"max_results", {{"type", "integer"}, {"description", "Maximum results, 1-10"}}}}},
            {"required", json::array({"query"})}};
    }
    ToolResult execute(const json& arguments) const override { return plugin_.search(arguments); }

private:
    WebPlugin& plugin_;
};

bool parse_positive_int(const std::string& value, int min, int max, int& out) {
    try {
        const int parsed = std::stoi(value);
        if (parsed < min || parsed > max)
            return false;
        out = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

WebPlugin::WebPlugin() : transport_(std::make_shared<web::CurlWebTransport>()) {}

WebPlugin::WebPlugin(std::shared_ptr<web::WebTransport> transport)
    : transport_(std::move(transport)) {}

bool WebPlugin::initialize(const PluginContext& ctx) {
    tools_ = &ctx.tools;
    const std::filesystem::path base =
        std::filesystem::weakly_canonical(agent::global_config_dir());
    const std::filesystem::path file =
        std::filesystem::weakly_canonical(base / "plugins" / "web" / "web.conf");
    const std::string prefix = base.generic_string() + "/";
    if (file.generic_string().rfind(prefix, 0) == 0) {
        settings_path_ = file.string();
        load_settings();
    }
    return true;
}

void WebPlugin::load_settings() {
    std::ifstream in(settings_path_);
    if (!in)
        return;
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "provider" && (value == "builtin" || value == "cdp" || value == "off"))
            settings_.provider = value;
        else if (key == "timeout_s")
            (void)parse_positive_int(value, 1, 300, settings_.timeout_s);
        else if (key == "max_bytes")
            (void)parse_positive_int(value, 1024, 10 * 1024 * 1024, settings_.max_bytes);
        else if (key == "search_url" && !value.empty())
            settings_.search_url = value;
    }
}

void WebPlugin::save_settings() const {
    if (settings_path_.empty())
        return;
    std::error_code ec;
    const std::filesystem::path file(settings_path_);
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(settings_path_, std::ios::trunc);
    if (!out)
        return;
    out << "provider=" << settings_.provider << "\n"
        << "timeout_s=" << settings_.timeout_s << "\n"
        << "max_bytes=" << settings_.max_bytes << "\n"
        << "search_url=" << settings_.search_url << "\n";
}

bool WebPlugin::set_provider(const std::string& provider) {
    if (provider != "builtin" && provider != "cdp" && provider != "off")
        return false;
    if (provider == "cdp" && (!tools_ || tools_->find("plugin_cdp_navigate") == nullptr))
        return false;
    settings_.provider = provider;
    save_settings();
    return true;
}

bool WebPlugin::set_timeout(const std::string& value) {
    if (!parse_positive_int(value, 1, 300, settings_.timeout_s))
        return false;
    save_settings();
    return true;
}

bool WebPlugin::set_max_bytes(const std::string& value) {
    if (!parse_positive_int(value, 1024, 10 * 1024 * 1024, settings_.max_bytes))
        return false;
    save_settings();
    return true;
}

bool WebPlugin::set_search_url(const std::string& value) {
    std::string error;
    if (!web::safe_web_url(value, error))
        return false;
    settings_.search_url = value;
    save_settings();
    return true;
}

std::optional<std::string> WebPlugin::provider_error() const {
    if (settings_.provider == "builtin")
        return std::nullopt;
    if (settings_.provider == "off")
        return "web provider is off; /set web provider builtin|cdp";
    if (!tools_ || tools_->find("plugin_cdp_navigate") == nullptr)
        return "web provider is cdp but the cdp plugin is not active; enable /plugin cdp or "
               "/set web provider builtin";
    return "web provider is cdp; use the cdp plugin's browser tools";
}

ToolResult WebPlugin::fetch(const json& arguments) const {
    if (auto error = provider_error())
        return ToolResult{false, "", *error};
    const std::string url = arguments.value("url", "");
    if (url.empty())
        return ToolResult{false, "", "url is required"};
    web::WebSettings settings = settings_;
    const int requested = arguments.value("max_bytes", settings.max_bytes);
    settings.max_bytes = std::clamp(requested, 1024, 10 * 1024 * 1024);
    web::WebClient client(settings, transport_);
    std::string error;
    const auto text = client.fetch(url, error);
    if (!text)
        return ToolResult{false, "", error};
    return ToolResult{true, *text, ""};
}

ToolResult WebPlugin::search(const json& arguments) const {
    if (auto error = provider_error())
        return ToolResult{false, "", *error};
    const std::string query = arguments.value("query", "");
    if (query.empty())
        return ToolResult{false, "", "query is required"};
    const int max_results = std::clamp(arguments.value("max_results", 8), 1, 10);
    web::WebClient client(settings_, transport_);
    std::string error;
    const auto results = client.search(query, max_results, error);
    if (results.empty())
        return ToolResult{false, "", error};
    std::ostringstream out;
    for (const auto& result : results)
        out << result.title << "\n" << result.url << "\n" << result.snippet << "\n\n";
    return ToolResult{true, out.str(), ""};
}

std::vector<PluginSettingSpec> WebPlugin::settings_specs() {
    PluginSettingSpec provider;
    provider.key = "provider";
    provider.help = "Web provider: builtin, cdp or off";
    provider.type = "choice";
    provider.choices = {"builtin", "cdp", "off"};
    provider.value = settings_.provider;
    provider.get = [this] { return settings_.provider; };
    provider.set = [this](const std::string& value) { return set_provider(value); };

    PluginSettingSpec timeout;
    timeout.key = "timeout_s";
    timeout.help = "HTTP timeout in seconds";
    timeout.type = "int";
    timeout.value = std::to_string(settings_.timeout_s);
    timeout.get = [this] { return std::to_string(settings_.timeout_s); };
    timeout.set = [this](const std::string& value) { return set_timeout(value); };

    PluginSettingSpec max_bytes;
    max_bytes.key = "max_bytes";
    max_bytes.help = "Response text cap in bytes";
    max_bytes.type = "int";
    max_bytes.value = std::to_string(settings_.max_bytes);
    max_bytes.get = [this] { return std::to_string(settings_.max_bytes); };
    max_bytes.set = [this](const std::string& value) { return set_max_bytes(value); };

    PluginSettingSpec search_url;
    search_url.key = "search_url";
    search_url.help = "Search endpoint; {query} is replaced with the encoded query";
    search_url.type = "string";
    search_url.value = settings_.search_url;
    search_url.get = [this] { return settings_.search_url; };
    search_url.set = [this](const std::string& value) { return set_search_url(value); };
    return {provider, timeout, max_bytes, search_url};
}

std::unique_ptr<Capability> WebPlugin::tool_capability() {
    return std::make_unique<ToolCapability>(
        "web",
        [this](PluginServices&) {
            std::vector<std::unique_ptr<Tool>> tools;
            tools.push_back(std::make_unique<WebFetchTool>(*this));
            tools.push_back(std::make_unique<WebSearchTool>(*this));
            return tools;
        },
        ToolCapability::Meta{{{"web_fetch", {"fetching", ToolRole::Search}},
                              {"web_search", {"searching", ToolRole::Search}}}});
}

std::vector<std::unique_ptr<Capability>> WebPlugin::capabilities() {
    std::vector<std::unique_ptr<Capability>> caps;
    caps.push_back(std::make_unique<SettingsCapability>(settings_specs()));
    caps.push_back(tool_capability());
    caps.push_back(
        make_tool_doc_capability("web_doc", tool_doc_priority::kWeb, "prompts/tools/web.md"));
    return caps;
}

} // namespace agent::plugins
