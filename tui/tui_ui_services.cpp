#include "tui_ui_services.h"

#include <future>
#include <utility>

namespace tui {

AskAnswer TuiUiServices::ask(AgentEvent::AskKind kind, const agent::AskSpec& spec) {
    auto ev = std::make_shared<AgentEvent>();
    ev->type = AgentEvent::Ask;
    ev->ask_kind = kind;
    ev->ask_spec = spec;
    ev->ask_promise = std::make_shared<std::promise<AskAnswer>>();
    return request_(std::move(ev));
}

std::string TuiUiServices::ask_text(const agent::AskSpec& spec) {
    return ask(AgentEvent::AskText, spec).text;
}

std::string TuiUiServices::ask_secret(const agent::AskSpec& spec) {
    return ask(AgentEvent::AskSecret, spec).text;
}

int TuiUiServices::choose(const agent::ChooseSpec& spec) {
    auto ev = std::make_shared<AgentEvent>();
    ev->type = AgentEvent::Ask;
    ev->ask_kind = AgentEvent::AskChoose;
    ev->choose_spec = spec;
    ev->ask_promise = std::make_shared<std::promise<AskAnswer>>();
    return request_(std::move(ev)).index;
}

bool TuiUiServices::confirm(const agent::ConfirmSpec& spec) {
    auto ev = std::make_shared<AgentEvent>();
    ev->type = AgentEvent::Ask;
    ev->ask_kind = AgentEvent::AskConfirm;
    ev->confirm_spec = spec;
    ev->ask_promise = std::make_shared<std::promise<AskAnswer>>();
    return request_(std::move(ev)).confirmed;
}

void TuiUiServices::notify(agent::UiLevel level, const std::string& message) {
    if (notify_) notify_(level, message);
}

void TuiUiServices::post_to_ui(std::function<void()> work) {
    if (post_) post_(std::move(work));
}

} // namespace tui
