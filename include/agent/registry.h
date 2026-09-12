
#ifndef AGENT_REGISTRY_H
#define AGENT_REGISTRY_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "agent/tool.h"

namespace agent {

// What a tool does, in the harness's vocabulary. Coarser than the tool's own
// behaviour and deliberately so: it is what the audit reasons about, not what
// the tool implements. A tool that only fits its own description is `Other`,
// which is a complete answer - an audit cannot reason about a category nobody
// else shares.
enum class ToolRole : std::uint8_t {
    Other,
    Read,      // brings workspace content back
    Write,     // changes workspace content
    Search,    // locates content across the workspace
    Execute,   // runs a process or command
    Plan,      // records intent for the session
    Delegate,  // hands work to a sub-agent
};

std::string to_string(ToolRole role);
// Parse a role name ("read"); unknown yields Other.
ToolRole parse_tool_role(const std::string& name);

// Harness-facing metadata about a tool, kept beside it in the registry rather
// than on the Tool port: this is vocabulary the harness owns ("searching",
// "search"), not behaviour the tool performs. A tool that describes its own
// *invocation* does that through Tool::summarize(args); this is what a status
// line needs when it does not know the tool, and what the toolset audit uses to
// see the shape of the whole set.
struct ToolMeta {
    // Gerund shown while the tool runs. Empty falls back to a generic word, so
    // a tool that declares nothing still renders — never a blank.
    std::string verb;
    // What the tool does. Used to tell whether the enabled set can still do
    // the work at all, and to find two tools competing for the same job.
    ToolRole role = ToolRole::Other;
};

// Owns the set of available tools and their metadata. The wire payload the
// model receives (tools[] in a provider's shape) is built by the resolved
// Dialect from these tools — see `docs/spec/llm-client/dialect.md`.
// Tools are registered by the host (library or UI) and looked up by name when
// the model requests an invocation.
//
// Thread safety: register/find/snapshot_tools/unregister are mutex-guarded.
// Tool dispatch runs on worker threads while the host may register tools (MCP
// connects, plugin enables, skill refresh) and parallel sub-agents construct
// their own agents — unsynchronized vector mutation would be a data race.
// snapshot_tools() returns an independent snapshot of the owned tools, safe
// to use after the registry lock is released; find() returns a shared lease
// that survives a concurrent unregister/replacement.
class ToolRegistry {
public:
    // Register a tool under an owner. `owner` is the contributing plugin's id;
    // empty means the host. Ownership is what makes unwinding precise: a
    // plugin's removal can only ever reach its own tools, even when two plugins
    // contribute the same name (the later registration replaces the earlier
    // one, and the replacement carries its own owner).
    void register_tool(std::unique_ptr<Tool> tool, std::string owner = {}, ToolMeta meta = {});
    // Shared lease: the caller keeps the tool alive across a concurrent
    // unregister/replacement (dispatch holds the lease through execute()).
    std::shared_ptr<Tool> find(const std::string& name) const;

    // The metadata recorded for `name`; default-constructed when the tool is
    // unknown or declared none. The UI calls this instead of keeping its own
    // name→verb table, which is how a plugin-contributed tool used to render
    // generically.
    ToolMeta meta_for(const std::string& name) const;

    bool empty() const;

    // Snapshot of the owned tools, consistent under the registry lock.
    std::vector<std::shared_ptr<Tool>> snapshot_tools() const;

    // Remove every tool whose name starts with `prefix` (e.g. "mcp_github_").
    // Returns the number removed.
    size_t unregister_tools_with_prefix(const std::string& prefix);

    // Remove exactly one tool by name. Returns true when it existed. This is
    // the host's escape hatch: it does not consult ownership.
    bool remove_tool(const std::string& name);

    // Remove `name` only when `owner` is its recorded contributor. Returns true
    // when it was removed. This is the ledger's unwinder, and it is what makes
    // unwinding safe under a name collision: a plugin can only ever remove its
    // own tool, never a neighbour's.
    bool remove_owned_tool(const std::string& name, const std::string& owner);

private:
    struct Entry {
        std::shared_ptr<Tool> tool;
        std::string owner;
        ToolMeta meta;
    };

    mutable std::mutex mtx_;
    std::vector<Entry> tools_;
};

} // namespace agent

#endif // AGENT_REGISTRY_H
