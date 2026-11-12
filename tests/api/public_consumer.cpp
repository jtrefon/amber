// An external-style translation unit for the 1.0 plugin API.
//
// It includes only the promised headers and uses the stable types, constructors
// and registries. `make api-check` compiles it with the normal build flags, so
// a removal or a signature change that the text snapshot might mis-read still
// stops compiling. It is not linked and not part of the test suite: the
// compiler is the oracle.

#include "agent/events.h"
#include "agent/extensions.h"
#include "agent/plugin.h"
#include "agent/plugin_capability.h"
#include "agent/plugin_core.h"

#include <memory>
#include <string>
#include <vector>

namespace {

class SamplePlugin : public agent::IPlugin {
public:
    std::string id() const override { return "sample"; }
    std::string version() const override { return "1.0.0"; }
    std::string name() const override { return "Sample"; }

    bool initialize(const agent::PluginContext& ctx) override {
        (void)ctx.tools;
        (void)ctx.config;
        (void)ctx.workspace;
        return true;
    }

    void shutdown() override {}

    std::vector<std::unique_ptr<agent::Capability>> capabilities() override {
        std::vector<std::unique_ptr<agent::Capability>> out;
        out.push_back(std::make_unique<agent::PromptBlockCapability>(
            "hint", 10, [] { return std::string("hint"); }));
        out.push_back(std::make_unique<agent::StatusSegmentCapability>(
            "probe", 10, 50, [](const agent::StatusSnapshot& snapshot) {
                return agent::StatusText{snapshot.model, agent::StatusTone::Dim};
            }));
        return out;
    }
};

[[maybe_unused]] void use(const agent::PluginContext& ctx, agent::PluginServices& services,
                          agent::PromptRegistry& prompts, agent::StatusRegistry& status,
                          agent::PanelRegistry& panels, agent::CommandRegistry& commands,
                          agent::KeybindingRegistry& keys, agent::SettingsRegistry& settings,
                          agent::SearchBackendRegistry& search, agent::WalletRegistry& wallets,
                          agent::PluginLedger& ledger, agent::EventBus& bus) {
    SamplePlugin plugin;
    (void)plugin.id();
    auto declared = plugin.capabilities();
    (void)declared.size();
    (void)ctx.event_bus;
    (void)prompts.items();
    (void)prompts.size();
    (void)status.items();
    (void)panels.items();
    (void)commands.items();
    (void)keys.items();
    (void)settings.items();
    (void)search.items();
    (void)wallets.items();
    (void)ledger.size("sample");
    (void)services.prompts();

    agent::Events events(bus);
    auto subscription = events.subscribe<agent::TurnStartedEvent>(
        [](const agent::TurnStartedEvent& event) { (void)event.prompt.size(); });
    (void)subscription;

    agent::PluginInfo info;
    (void)info.state;
    agent::PluginManifest manifest;
    (void)manifest.protocol_version;
    agent::Contribution contribution;
    (void)contribution.kind;
    agent::InstallResult result;
    (void)result.ok;
    agent::ExtensionItem item;
    (void)item.name;
}

} // namespace
