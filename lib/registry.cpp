
#include "agent/registry.h"

namespace agent {

void ToolRegistry::register_tool(std::unique_ptr<Tool> tool) {
    std::scoped_lock lk(mtx_);
    // Idempotent by name: re-registration (a second Agent, a skills
    // re-discovery) replaces the earlier instance instead of duplicating
    // it — duplicate names in the tools[] schema are rejected by strict
    // servers ("Tool names must be unique").
    std::shared_ptr<Tool> owned(std::move(tool));
    const std::string name = owned->name();
    for (auto& t : tools_) {
        if (t->name() == name) {
            t = std::move(owned);
            return;
        }
    }
    tools_.push_back(std::move(owned));
}

std::shared_ptr<Tool> ToolRegistry::find(const std::string& name) const {
    std::scoped_lock lk(mtx_);
    for (const auto& t : tools_)
        if (t->name() == name) return t;
    return nullptr;
}

bool ToolRegistry::empty() const {
    std::scoped_lock lk(mtx_);
    return tools_.empty();
}

size_t ToolRegistry::unregister_tools_with_prefix(const std::string& prefix) {
    std::scoped_lock lk(mtx_);
    size_t removed = 0;
    for (auto it = tools_.begin(); it != tools_.end();) {
        if ((*it)->name().rfind(prefix, 0) == 0) {
            it = tools_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

json ToolRegistry::schema() const {
    std::scoped_lock lk(mtx_);
    json arr = json::array();
    for (const auto& t : tools_) {
        arr.push_back({
            {"type", "function"},
            {"function", {
                {"name", t->name()},
                {"description", t->description()},
                {"parameters", t->parameters_schema()}
            }}
        });
    }
    return arr;
}

std::vector<std::shared_ptr<Tool>> ToolRegistry::snapshot_tools() const {
    std::scoped_lock lk(mtx_);
    return tools_;
}

} // namespace agent
