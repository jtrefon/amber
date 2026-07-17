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
};

// A tool advertised to the LLM. The schema field is the JSON object placed
// into the OpenAI tools[] array (name, description, parameters).
class Tool {
public:
    virtual ~Tool() = default;

    // Stable identifier used by the model to invoke the tool.
    virtual std::string name() const = 0;

    // Markdown or plain description surfaced in prompts / tool advertising.
    virtual std::string description() const = 0;

    // OpenAI-compatible function schema (parameters object). The "name" and
    // top-level "description" are filled by the registry from name()/description().
    virtual json parameters_schema() const = 0;

    // Execute with the arguments supplied by the model.
    virtual ToolResult execute(const json& arguments) const = 0;
};

} // namespace agent

#endif // AGENT_TOOL_H
