#ifndef AMBER_TUI_PROMPT_PORT_H
#define AMBER_TUI_PROMPT_PORT_H

#include <optional>
#include <string>

namespace tui {

// Port: prompt submission. The domain core uses this to send prompts
// to the agent and manage the pending-prompt queue.
class PromptPort {
public:
    virtual ~PromptPort() = default;
    virtual void send_async(const std::string& text) = 0;
    virtual void queue_prompt(const std::string& text) = 0;
    virtual std::optional<std::string> take_pending_prompt() = 0;
};

} // namespace tui

#endif // AMBER_TUI_PROMPT_PORT_H
