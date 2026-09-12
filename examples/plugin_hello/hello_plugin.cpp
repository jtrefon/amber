#include "hello_plugin.h"

#include "agent/extensions.h"
#include "agent/tool.h"

#include <nlohmann/json.hpp>

namespace agent::examples {

namespace {

// The tool the plugin contributes. The plugin never registers it directly,
// it is handed to the runtime through ToolCapability, which records it in the
// ledger so disabling the plugin removes it again.
class GreetTool : public Tool {
public:
    std::string name() const noexcept override { return "greet"; }
    std::string description() const noexcept override { return "Greet someone by name."; }

    json parameters_schema() const override {
        return json{{"type", "object"},
                    {"properties", {{"name", {{"type", "string"}}}}},
                    {"required", json::array({"name"})}};
    }

    ToolResult execute(const json& args) const override {
        const std::string who = args.value("name", "world");
        return {true, "Hello, " + who + "!", "", json{}};
    }
};

} // namespace

bool HelloPlugin::initialize(const PluginContext&) { return true; }

std::vector<std::unique_ptr<Capability>> HelloPlugin::capabilities() {
    std::vector<std::unique_ptr<Capability>> caps;
    caps.push_back(std::make_unique<ToolCapability>(
        "greet", [](PluginServices&) -> std::vector<std::unique_ptr<Tool>> {
            std::vector<std::unique_ptr<Tool>> tools;
            tools.push_back(std::make_unique<GreetTool>());
            return tools;
        }));
    return caps;
}

} // namespace agent::examples
