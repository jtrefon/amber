#ifndef AGENT_PLUGINS_WEB_WEB_PLUGIN_H
#define AGENT_PLUGINS_WEB_WEB_PLUGIN_H

#include "web_client.h"

#include "agent/extensions.h"
#include "agent/plugin_core.h"

#include <memory>
#include <string>
#include <vector>

namespace agent::plugins {

class WebPlugin : public IPlugin {
public:
    WebPlugin();
    explicit WebPlugin(std::shared_ptr<web::WebTransport> transport);

    std::string id() const override { return "web"; }
    std::string version() const override { return "0.1.0"; }
    std::string name() const override { return "Web access"; }
    std::string description() const override {
        return "Built-in HTTP web fetch/search, conflicting with the external CDP browser.";
    }
    std::string category() const override { return plugin_category::kTools; }

    bool initialize(const PluginContext& ctx) override;
    void shutdown() override {}
    std::vector<std::unique_ptr<Capability>> capabilities() override;

    ToolResult fetch(const nlohmann::json& arguments) const;
    ToolResult search(const nlohmann::json& arguments) const;

    const web::WebSettings& settings() const noexcept { return settings_; }
    bool set_provider(const std::string& provider);
    bool set_timeout(const std::string& value);
    bool set_max_bytes(const std::string& value);
    bool set_search_url(const std::string& value);

private:
    std::optional<std::string> provider_error() const;
    std::vector<PluginSettingSpec> settings_specs();
    std::unique_ptr<Capability> tool_capability();
    void load_settings();
    void save_settings() const;

    web::WebSettings settings_;
    std::shared_ptr<web::WebTransport> transport_;
    const ToolRegistry* tools_ = nullptr;
    std::string settings_path_;
};

} // namespace agent::plugins

#endif // AGENT_PLUGINS_WEB_WEB_PLUGIN_H
