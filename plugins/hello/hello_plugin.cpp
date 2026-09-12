#include "hello_plugin.h"

#include "agent/extensions.h"

#include <nlohmann/json.hpp>

namespace agent::plugins {

bool HelloPlugin::initialize(const PluginContext&) { return true; }

std::vector<std::unique_ptr<Capability>> HelloPlugin::capabilities() {
    // The namespace: one root ("hello") and its child nodes, in the same shape
    // completions.json uses. The host merges this into the command tree.
    CommandSpec spec;
    spec.root = "hello";
    spec.help = "Say hello (example plugin command)";
    spec.man = "Usage: /hello greet <name>, returns a greeting string.";
    spec.subtree = nlohmann::json::parse(R"({
        "greet": {
            "help": "Greet someone by name",
            "man": "Usage: /hello greet <name>"
        }
    })");

    // The logic, exposed as a callback. The leaf path ("greet") keys it; the
    // handler returns the text to display and the host prints it, so the plugin
    // stays free of any UI dependency.
    spec.handlers["greet"] = [](const std::string& arg) {
        return "Hello, " + (arg.empty() ? std::string("world") : arg) + "!";
    };

    std::vector<std::unique_ptr<Capability>> caps;
    caps.push_back(std::make_unique<CommandCapability>(std::move(spec)));
    return caps;
}

} // namespace agent::plugins
