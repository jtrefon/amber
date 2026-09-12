#ifndef AMBER_TUI_UI_SERVICES_H
#define AMBER_TUI_UI_SERVICES_H

#include "agent/ui_services.h"
#include "agent_event.h"

#include <functional>
#include <memory>
#include <string>

namespace tui {

// The TUI's implementation of the plugin-facing user-interaction port (§8).
//
// A plugin calls from its own thread; `request` posts the event to the UI
// thread's queue and blocks on the promise, exactly as the API-key prompt does.
// Nothing here touches ncurses: this class only builds requests and forwards
// fire-and-forget work.
class TuiUiServices : public agent::UiServices {
public:
    using Request = std::function<AskAnswer(std::shared_ptr<AgentEvent>)>;
    using Notify = std::function<void(agent::UiLevel, const std::string&)>;
    using Post = std::function<void(std::function<void()>)>;

    TuiUiServices(Request request, Notify notify, Post post)
        : request_(std::move(request)), notify_(std::move(notify)), post_(std::move(post)) {}

    std::string ask_text(const agent::AskSpec& spec) override;
    std::string ask_secret(const agent::AskSpec& spec) override;
    int choose(const agent::ChooseSpec& spec) override;
    bool confirm(const agent::ConfirmSpec& spec) override;
    void notify(agent::UiLevel level, const std::string& message) override;
    void post_to_ui(std::function<void()> work) override;

private:
    AskAnswer ask(AgentEvent::AskKind kind, const agent::AskSpec& spec);

    Request request_;
    Notify notify_;
    Post post_;
};

} // namespace tui

#endif // AMBER_TUI_UI_SERVICES_H
