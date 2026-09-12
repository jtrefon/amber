#ifndef AMBER_TUI_AGENT_EVENT_H
#define AMBER_TUI_AGENT_EVENT_H

#include <agent.h>
#include <agent/ui_services.h>

#include <future>
#include <memory>
#include <string>

namespace tui {

// The answer to a plugin's question: a string (ask_text/ask_secret), an index
// (choose, -1 = cancelled), or a verdict (confirm).
struct AskAnswer {
    std::string text;
    int index = -1;
    bool confirmed = false;
};

// Inter-thread event emitted by the agent worker and consumed on the UI
// thread during the main event loop.
struct AgentEvent {
    enum Type {
        Token,
        Reasoning,
        StateChange,
        ToolCall,
        ToolResult,
        Status,
        Stats,
        Assistant,
        Approval,
        ApiKey,
        Ask,
        Error,
        Done,
        CompressResult,
    };
    Type type;
    std::string text;
    agent::RunState state = agent::RunState::Idle;
    agent::Stats stats{};
    std::string tool_name;
    agent::ToolResult tool_result{};
    agent::json tool_args;
    std::string error_msg;
    agent::CompressionResult compress_result{};

    // Worker thread blocks on this promise until the UI thread
    // shows the approval dialog and resolves it.
    std::shared_ptr<std::promise<agent::Approval>> approval_promise;

    // Worker thread blocks on this promise until the UI thread shows the
    // API-key dialog and resolves it with the entered key (or "" on cancel).
    std::shared_ptr<std::promise<std::string>> api_key_promise;

    // A plugin's question (spec §8). The worker blocks on the promise while the
    // UI thread shows the matching modal; the answer carries whichever field
    // the question asked for. Same modal-deferral rules as approvals and keys.
    enum AskKind { AskText, AskSecret, AskChoose, AskConfirm };
    AskKind ask_kind = AskText;
    agent::AskSpec ask_spec;
    agent::ChooseSpec choose_spec;
    agent::ConfirmSpec confirm_spec;
    std::shared_ptr<std::promise<AskAnswer>> ask_promise;

    // Window the event belongs to (captured when the worker started); npos
    // means "the active window at drain time". Streaming and tool events are
    // delivered to their origin window so switching windows mid-run never
    // bleeds one conversation's output into another.
    size_t window_id = std::string::npos;
};

} // namespace tui

#endif // AMBER_TUI_AGENT_EVENT_H
