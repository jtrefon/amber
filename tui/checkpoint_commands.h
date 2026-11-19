#ifndef AMBER_TUI_CHECKPOINT_COMMANDS_H
#define AMBER_TUI_CHECKPOINT_COMMANDS_H

#include "agent/checkpoint_service.h"

#include <string>

namespace tui {

class SlashDispatcher;
class Tui;

// /session history|undo|rollback|diff: the user-facing side of the checkpoint
// store. Kept out of SlashDispatcher so the command table stays under its type
// cap and one domain is edited in one place.
class CheckpointCommands {
public:
    explicit CheckpointCommands(Tui& tui) : ui_(tui) {}

    void register_actions(SlashDispatcher& dispatcher);
    void history();
    void diff(const std::string& arg);
    void undo();
    void rollback(const std::string& arg);

private:
    bool require_chat() const;
    void report_restore(const agent::CheckpointChange& change, const char* verb);
    agent::CheckpointService service() const;

    Tui& ui_;
};

} // namespace tui

#endif // AMBER_TUI_CHECKPOINT_COMMANDS_H
