#ifndef AMBER_TEMPLATE_HELLO_PLUGIN_H
#define AMBER_TEMPLATE_HELLO_PLUGIN_H

// The copy-me template for core plugins (docs/spec/plugins/developer-guide.md).
//
// A minimal core plugin: one tool, declared as a capability and installed by
// the runtime's ledger. It is compiled and exercised by
// tests/example_plugin_test.cpp, so the template cannot rot.
//
// Copy this directory to plugins/<your-id>/, change the id, and replace the
// tool. It is deliberately not registered in make_bundled_plugins().

#include "agent/plugin_core.h"

#include <memory>
#include <string>
#include <vector>

namespace agent::templates {

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

} // namespace agent::templates

#endif // AMBER_TEMPLATE_HELLO_PLUGIN_H
