// The plugin-facing user-interaction port (spec §8, PLG-07).
//
// Two things must hold: a plugin's question reaches the host's implementation
// and the answer comes back to the caller, and a host that cannot ask fails
// closed instead of guessing. Hermetic - a recording fake stands in for the UI.

#include "agent/extensions.h"
#include "agent/plugin_runtime.h"
#include "agent/registry.h"
#include "agent/ui_services.h"
#include "agent/workspace.h"
#include "test_util.h"

#include <memory>
#include <string>
#include <vector>

using namespace agent;

namespace {

// Records what was asked, and answers with whatever the test set.
class FakeUi : public UiServices {
public:
    std::string text_answer = "the-answer";
    int choose_answer = 1;
    bool confirm_answer = true;

    std::string ask_text(const AskSpec& spec) override {
        text_specs.push_back(spec);
        return text_answer;
    }
    std::string ask_secret(const AskSpec& spec) override {
        secret_specs.push_back(spec);
        return text_answer;
    }
    int choose(const ChooseSpec& spec) override {
        choose_specs.push_back(spec);
        return choose_answer;
    }
    bool confirm(const ConfirmSpec& spec) override {
        confirm_specs.push_back(spec);
        return confirm_answer;
    }
    void notify(UiLevel level, const std::string& message) override {
        notifications.emplace_back(level, message);
    }
    void post_to_ui(std::function<void()> work) override {
        ++posted;
        if (work) work();
    }

    std::vector<AskSpec> text_specs;
    std::vector<AskSpec> secret_specs;
    std::vector<ChooseSpec> choose_specs;
    std::vector<ConfirmSpec> confirm_specs;
    std::vector<std::pair<UiLevel, std::string>> notifications;
    int posted = 0;
};

// A plugin that asks the user for a secret while contributing a tool, which is
// what a provider plugin's setup does.
class AskingPlugin : public IPlugin {
public:
    static inline std::string collected;

    std::string id() const override { return "asking"; }
    std::string version() const override { return "1.0.0"; }
    std::string name() const override { return "Asking plugin"; }

    bool initialize(const PluginContext&) override { return true; }
    void shutdown() override {}

    std::vector<std::unique_ptr<Capability>> capabilities() override {
        std::vector<std::unique_ptr<Capability>> caps;
        caps.push_back(std::make_unique<ToolCapability>(
            "asking", [](PluginServices& services) {
                collected = services.ui->ask_secret(AskSpec{"Key needed", "Paste it", ""});
                services.ui->notify(UiLevel::Info, "asked for a key");
                services.ui->post_to_ui([] {});
                return std::vector<std::unique_ptr<Tool>>{};
            }));
        return caps;
    }
};

} // namespace

TEST(null_ui_services_fails_closed) {
    NullUiServices ui;
    ASSERT_EQ(ui.ask_text(AskSpec{"t", "p", "seed"}), std::string());
    ASSERT_EQ(ui.ask_secret(AskSpec{"t", "p", "seed"}), std::string());
    ASSERT_EQ(ui.choose(ChooseSpec{"t", {"a", "b"}, 0}), -1);
    ASSERT_FALSE(ui.confirm(ConfirmSpec{"t", "m"}));
    ui.notify(UiLevel::Error, "ignored");   // must not throw
    ui.post_to_ui([] {});                   // must not run: there is no UI thread
}

// The plugin-facing default is the null implementation, so a capability can ask
// without checking first and a host that attached nothing cannot be surprised.
TEST(plugin_services_default_to_the_null_ui) {
    ToolRegistry tools;
    PromptRegistry prompts;
    StatusRegistry status;
    PanelRegistry panels;
    WalletRegistry wallets;
    EventBus bus;
    PluginServices services(tools, prompts, status, panels, wallets, bus);

    ASSERT_TRUE(services.ui != nullptr);
    ASSERT_EQ(services.ui->ask_secret(AskSpec{"t", "p", ""}), std::string());
    ASSERT_FALSE(services.ui->confirm(ConfirmSpec{"t", "m"}));
}

TEST(host_services_a_plugin_asks_and_gets_the_answer) {
    FakeUi ui;
    ui.text_answer = "sk-secret";

    ToolRegistry tools;
    Config cfg;
    Workspace ws;
    PluginRuntime runtime(tools, cfg, ws);
    runtime.attach_ui_services(&ui);
    runtime.add(std::make_shared<AskingPlugin>());
    runtime.start();

    // The plugin asked through the port, the answer came back to its caller...
    ASSERT_EQ(AskingPlugin::collected, std::string("sk-secret"));
    // ...and the host saw the question it was asked, with the title and prompt
    // it was given rather than a generic one.
    ASSERT_EQ(ui.secret_specs.size(), 1u);
    ASSERT_EQ(ui.secret_specs[0].title, std::string("Key needed"));
    ASSERT_EQ(ui.secret_specs[0].prompt, std::string("Paste it"));
    // notify and post_to_ui reached the same host in the same install.
    ASSERT_EQ(ui.notifications.size(), 1u);
    ASSERT_TRUE(ui.notifications[0].first == UiLevel::Info);
    ASSERT_EQ(ui.posted, 1);
}

// A host with no UI attached must not leave a plugin blocked: the question is
// answered (with "no") rather than waiting for a user who is not there.
TEST(host_services_an_unattached_host_still_answers) {
    AskingPlugin::collected = "untouched";

    ToolRegistry tools;
    Config cfg;
    Workspace ws;
    PluginRuntime runtime(tools, cfg, ws);
    runtime.add(std::make_shared<AskingPlugin>());
    runtime.start();   // no attach_ui_services

    ASSERT_EQ(AskingPlugin::collected, std::string());
}

TEST(host_services_questions_reach_the_plugin_that_asked) {
    // Two plugins, one host: each question is answered by the same port, and
    // the answers differ per caller because the caller owns the call.
    FakeUi first;
    first.text_answer = "first";
    FakeUi second;
    second.text_answer = "second";

    for (FakeUi* ui : {&first, &second}) {
        ToolRegistry tools;
        Config cfg;
        Workspace ws;
        PluginRuntime runtime(tools, cfg, ws);
        runtime.attach_ui_services(ui);
        runtime.add(std::make_shared<AskingPlugin>());
        runtime.start();
    }
    ASSERT_EQ(first.secret_specs.size(), 1u);
    ASSERT_EQ(second.secret_specs.size(), 1u);
    ASSERT_EQ(AskingPlugin::collected, std::string("second"));
}
