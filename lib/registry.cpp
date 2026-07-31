
#include "agent/registry.h"

namespace agent {

void ToolRegistry::register_tool(std::unique_ptr<Tool> tool) {
    tools_.push_back(std::move(tool));
}

Tool* ToolRegistry::find(const std::string& name) const {
    for (const auto& t : tools_)
        if (t->name() == name) return t.get();
    return nullptr;
}

size_t ToolRegistry::unregister_tools_with_prefix(const std::string& prefix) {
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
