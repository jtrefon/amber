#include "agent/registry.h"

#include <algorithm>
#include <iterator>

namespace agent {

namespace {

constexpr const char* kRoleNames[] = {"other", "read", "write", "search",
                                      "execute", "plan", "delegate"};

} // namespace

std::string to_string(ToolRole role) {
    const auto i = static_cast<std::size_t>(role);
    return i < std::size(kRoleNames) ? kRoleNames[i] : "other";
}

ToolRole parse_tool_role(const std::string& name) {
    for (std::size_t i = 0; i < std::size(kRoleNames); ++i) {
        if (name == kRoleNames[i]) return static_cast<ToolRole>(i);
    }
    return ToolRole::Other;
}

void ToolRegistry::register_tool(std::unique_ptr<Tool> tool, std::string owner, ToolMeta meta) {
    std::scoped_lock lk(mtx_);
    // Idempotent by name: re-registration (a second Agent, a skills
    // re-discovery) replaces the earlier instance instead of duplicating
    // it — duplicate names in the tools[] schema are rejected by strict
    // servers ("Tool names must be unique"). The replacement takes over the
    // registration entirely, owner included, so a plugin whose name was
    // superseded owns nothing it did not put there.
    std::shared_ptr<Tool> owned(std::move(tool));
    const std::string name = owned->name();
    for (auto& entry : tools_) {
        if (entry.tool->name() == name) {
            entry.tool = std::move(owned);
            entry.owner = std::move(owner);
            entry.meta = std::move(meta);
            return;
        }
    }
    tools_.push_back(Entry{std::move(owned), std::move(owner), std::move(meta)});
}

ToolMeta ToolRegistry::meta_for(const std::string& name) const {
    std::scoped_lock lk(mtx_);
    for (const auto& entry : tools_) {
        if (entry.tool->name() == name)
            return entry.meta;
    }
    return {};
}

bool ToolRegistry::remove_tool(const std::string& name) {
    std::scoped_lock lk(mtx_);
    for (auto it = tools_.begin(); it != tools_.end(); ++it) {
        if (it->tool->name() == name) {
            tools_.erase(it);
            return true;
        }
    }
    return false;
}

bool ToolRegistry::remove_owned_tool(const std::string& name, const std::string& owner) {
    std::scoped_lock lk(mtx_);
    for (auto it = tools_.begin(); it != tools_.end(); ++it) {
        if (it->tool->name() == name && it->owner == owner) {
            tools_.erase(it);
            return true;
        }
    }
    return false;
}

std::shared_ptr<Tool> ToolRegistry::find(const std::string& name) const {
    std::scoped_lock lk(mtx_);
    for (const auto& entry : tools_)
        if (entry.tool->name() == name)
            return entry.tool;
    return nullptr;
}

bool ToolRegistry::empty() const {
    std::scoped_lock lk(mtx_);
    return tools_.empty();
}

size_t ToolRegistry::unregister_tools_with_prefix(const std::string& prefix) {
    std::scoped_lock lk(mtx_);
    const auto first = std::remove_if(tools_.begin(), tools_.end(), [&prefix](const Entry& e) {
        return e.tool->name().rfind(prefix, 0) == 0;
    });
    const auto removed = static_cast<size_t>(std::distance(first, tools_.end()));
    tools_.erase(first, tools_.end());
    return removed;
}

std::vector<std::shared_ptr<Tool>> ToolRegistry::snapshot_tools() const {
    std::scoped_lock lk(mtx_);
    std::vector<std::shared_ptr<Tool>> snapshot;
    snapshot.reserve(tools_.size());
    for (const auto& entry : tools_)
        snapshot.push_back(entry.tool);
    return snapshot;
}

} // namespace agent
