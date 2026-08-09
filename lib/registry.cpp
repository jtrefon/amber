
#include "agent/registry.h"

namespace agent {

void ToolRegistry::register_tool(std::unique_ptr<Tool> tool) {
    std::scoped_lock lk(mtx_);
    // Idempotent by name: re-registration (a second Agent, a skills
    // re-discovery) replaces the earlier instance instead of duplicating
    // it — duplicate names in the tools[] schema are rejected by strict
    // servers ("Tool names must be unique").
    const std::string name = tool->name();
    for (auto& t : tools_) {
        if (t->name() == name) {
            t = std::move(tool);
            return;
        }
    }
    tools_.push_back(std::move(tool));
}

Tool* ToolRegistry::find(const std::string& name) const {
    std::scoped_lock lk(mtx_);
    for (const auto& t : tools_)
        if (t->name() == name) return t.get();
    return nullptr;
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

} // namespace agent
