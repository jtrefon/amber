#ifndef AMBER_TUI_AGENT_EVENT_H
#define AMBER_TUI_AGENT_EVENT_H

#include <agent.h>

#include <future>
#include <memory>
#include <string>

namespace tui {

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

    // Window the event belongs to (captured when the worker started); npos
    // means "the active window at drain time". Streaming and tool events are
    // delivered to their origin window so switching windows mid-run never
    // bleeds one conversation's output into another.
    size_t window_id = std::string::npos;
};

} // namespace tui

#endif // AMBER_TUI_AGENT_EVENT_H
