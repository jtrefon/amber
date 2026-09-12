#ifndef AMBER_EXAMPLE_HELLO_PLUGIN_H
#define AMBER_EXAMPLE_HELLO_PLUGIN_H

// Reference example for docs/spec/plugins/developer-guide.md.
//
// A minimal core plugin: one tool, declared as a capability and installed by
// the runtime's ledger. It is compiled and exercised by
// tests/example_plugin_test.cpp, so the guide's anatomy cannot rot.
//
// Copy this directory, change the id, and replace the tool.

#include "agent/plugin_core.h"

#include <memory>
#include <string>
#include <vector>

namespace agent::examples {

class HelloPlugin : public IPlugin {
public:
    std::string id() const override { return "hello"; }
    std::string version() const override { return "1.0.0"; }
    std::string name() const override { return "Hello"; }
    std::string description() const override { return "Greets people by name."; }
    std::string category() const override { return plugin_category::kTools; }

    bool initialize(const PluginContext& ctx) override;
    void shutdown() override {}

    std::vector<std::unique_ptr<Capability>> capabilities() override;
};

} // namespace agent::examples

#endif // AMBER_EXAMPLE_HELLO_PLUGIN_H
