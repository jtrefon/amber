// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

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
