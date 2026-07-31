
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
    bool empty() const noexcept { return tools_.empty(); }

    // Build the tools[] payload for the chat/completions request.
    json schema() const;

    const std::vector<std::unique_ptr<Tool>>& tools() const { return tools_; }

    // Remove every tool whose name starts with `prefix` (e.g. "mcp_github_").
    // Returns the number removed.
    size_t unregister_tools_with_prefix(const std::string& prefix);

private:
    std::vector<std::unique_ptr<Tool>> tools_;
};

} // namespace agent

#endif // AGENT_REGISTRY_H
