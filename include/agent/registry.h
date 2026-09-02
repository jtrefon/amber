
#ifndef AGENT_REGISTRY_H
#define AGENT_REGISTRY_H

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "agent/tool.h"

namespace agent {

// Owns the set of available tools and renders their OpenAI-compatible schema.
// Tools are registered by the host (library or UI) and looked up by name when
// the model requests an invocation.
//
// Thread safety: register/find/schema/unregister are mutex-guarded. Tool
// dispatch runs on worker threads while the host may register tools (MCP
// connects, plugin enables, skill refresh) and parallel sub-agents construct
// their own agents — unsynchronized vector mutation would be a data race.
// snapshot_tools() returns an independent snapshot of the owned tools, safe
// to use after the registry lock is released; find() returns a shared lease
// that survives a concurrent unregister/replacement.
class ToolRegistry {
public:
    void register_tool(std::unique_ptr<Tool> tool);
    // Shared lease: the caller keeps the tool alive across a concurrent
    // unregister/replacement (dispatch holds the lease through execute()).
    std::shared_ptr<Tool> find(const std::string& name) const noexcept;
    bool empty() const noexcept;

    // Build the tools[] payload for the chat/completions request.
    json schema() const;

    // Snapshot of the owned tools, consistent under the registry lock.
    std::vector<std::shared_ptr<Tool>> snapshot_tools() const noexcept;

    // Remove every tool whose name starts with `prefix` (e.g. "mcp_github_").
    // Returns the number removed.
    size_t unregister_tools_with_prefix(const std::string& prefix);

private:
    mutable std::mutex mtx_;
    std::vector<std::shared_ptr<Tool>> tools_;
};

} // namespace agent

#endif // AGENT_REGISTRY_H
