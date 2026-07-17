// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AGENT_REGISTRY_H
#define AGENT_REGISTRY_H

#include <vector>
#include <memory>
#include <string>
#include "agent/tool.h"

namespace agent {

// Owns the set of available tools and renders their OpenAI-compatible schema.
// Tools are registered by the host (library or UI) and looked up by name when
// the model requests an invocation.
class ToolRegistry {
public:
    void register_tool(std::unique_ptr<Tool> tool);
    Tool* find(const std::string& name) const;
    bool empty() const { return tools_.empty(); }

    // Build the tools[] payload for the chat/completions request.
    json schema() const;

    const std::vector<std::unique_ptr<Tool>>& tools() const { return tools_; }

private:
    std::vector<std::unique_ptr<Tool>> tools_;
};

} // namespace agent

#endif // AGENT_REGISTRY_H
