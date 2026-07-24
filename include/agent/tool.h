// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AGENT_TOOL_H
#define AGENT_TOOL_H

#include <string>
#include <nlohmann/json.hpp>

namespace agent {

using json = nlohmann::json;

// Result of executing a tool. `ok` distinguishes success from error so the
// agent loop can relay failures back to the model without crashing.
struct ToolResult {
    bool ok = true;
    std::string output;   // human/model readable payload
    std::string error;    // populated when ok == false
    json meta;            // structured metadata (lines, exit, duration, etc.)
};

// A tool advertised to the LLM. The schema field is the JSON object placed
// into the OpenAI tools[] array (name, description, parameters).
class Tool {
public:
    virtual ~Tool() = default;

    // Stable identifier used by the model to invoke the tool.
    virtual std::string name() const noexcept = 0;

    // Markdown or plain description surfaced in prompts / tool advertising.
    virtual std::string description() const noexcept = 0;

    // OpenAI-compatible function schema (parameters object). The "name" and
    // top-level "description" are filled by the registry from name()/description().
    virtual json parameters_schema() const = 0;

    // Whether invoking this tool requires explicit user approval before it runs.
    // Side-effecting or dangerous tools (e.g. running shell commands) return
    // true so the host can gate them behind a confirmation prompt. Read-only
    // tools stay false. The agent loop consults AgentHooks::on_approval only for
    // tools that opt in here.
    virtual bool requires_approval() const noexcept { return false; }

    // Whether this tool is read-only (safe to run in read mode).
    // Read tools (search, grep, read) return true; write tools (write, edit,
    // bash) return false. In read mode, only read-only tools are dispatched.
    virtual bool is_read_only() const noexcept { return false; }

    // A short, human-readable summary of what this specific invocation will do,
    // shown in approval prompts (e.g. the command line for a shell tool).
    // Defaults to the tool name; override to surface the concrete action.
    virtual std::string summarize(const json& /*arguments*/) const {
        return name();
    }

    // Execute with the arguments supplied by the model.
    virtual ToolResult execute(const json& arguments) const = 0;
};

} // namespace agent

#endif // AGENT_TOOL_H
