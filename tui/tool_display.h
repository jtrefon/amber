
#ifndef AMBER_TUI_TOOL_DISPLAY_H
#define AMBER_TUI_TOOL_DISPLAY_H

#include <cstddef>
#include <string>

#include "agent/agent.h"
#include "agent/llm.h"
#include "tui/rich.h"

namespace tui::tool_display {

// The activity word leading the working indicator. Priority: an active tool
// (non-empty name) -> the verb the tool declared in the registry
// (reading/writing/searching/hacking/...); MCP tools -> "calling"; otherwise
// the run-state word (thinking/talking/waiting/retrying); otherwise "working".
// "waiting" is produced only when no tool is in flight.
//
// The verb comes from the registry, not a table here: a tool the harness has
// never heard of — a plugin's — gets its own word rather than a generic one.
// Pure so the label logic is unit-testable without ncurses.
std::string activity_verb(bool compressing, agent::RunState state, const std::string& running_tool,
                          const agent::ToolRegistry& registry);

// Human-readable description of a tool call for the scrollback line.
// bash -> the command verbatim (no tool name); read/write -> path;
// search -> pattern (+ path); a tool that describes its own invocation ->
// that; anything else -> name + truncated args.
std::string describe_tool_call(const std::string& name, const agent::json& args,
                               const agent::ToolRegistry& registry);

// Close an open tool line in place: prepend the open line's timestamp run
// (the dim P_REASONING run at index 0) to the summary so the single line
// keeps its timestamp. Returns the closed line.
rich::Line close_tool_line(const rich::Line& open, rich::Line summary);

// Elapsed-time label for the working indicator: 12s, 1m 05s, 1h 02m.
std::string elapsed_label(size_t secs);

// The full working indicator: "<frame> <verb> <elapsed>", plus an optional
// running-task description appended as " · <task>" (truncated to 40 cols).
// `verb` is a short activity word ("working", "thinking", "compressing",
// "searching", ...) chosen by the caller from the agent's current state.
std::string working_label(const std::string& frame, const std::string& verb, size_t elapsed_secs,
                          const std::string& task = {});

// Reasoning-strength badge text: "(<effort>)", composed inside the model
// bracket ([model(high)]). Always shown, including the off state.
std::string reasoning_badge(const std::string& effort);

// Context gauge value: the server-reported prompt_tokens when known (> 0),
// else the live chars/4 estimate; the streaming delta is always added.
// Single source so the gauge never switches metrics mid-turn.
long gauge_tokens(long server_prompt_tokens, long estimate, long streamed);

// One closed tool-result line: icon (check/cross) + description + arrow +
// "(N lines) preview" or "error: ...". The icon conveys success — no
// redundant "exit 0" text. Shared by the live ToolResult event and session
// restore so both render identically.
rich::Line result_line(const std::string& name, const agent::json& args, bool ok,
                       const std::string& output, const std::string& error,
                       const agent::ToolRegistry& registry);

} // namespace tui::tool_display

#endif // AMBER_TUI_TOOL_DISPLAY_H
