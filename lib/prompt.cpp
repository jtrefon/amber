// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/prompt.h"
#include "agent/registry.h"
#include <fstream>
#include <sstream>

namespace agent {

std::string load_prompt(const std::string& path) {
    if (path.empty()) return "";
    std::ifstream in(path);
    if (!in) return "";
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string render_tools_markdown(const ToolRegistry& registry) {
    std::stringstream out;
    out << "## Available Tools\n\n";
    out << "You may invoke the following tools by name. Each tool is called "
           "with a JSON object of arguments matching its schema.\n\n";
    for (const auto& t : registry.tools()) {
        out << "### `" << t->name() << "`\n\n";
        out << t->description() << "\n\n";
        json p = t->parameters_schema();
        if (p.contains("properties")) {
            out << "**Parameters:**\n\n";
            for (auto it = p["properties"].begin(); it != p["properties"].end(); ++it) {
                std::string type = it.value().value("type", "any");
                bool req = false;
                if (p.contains("required"))
                    for (const auto& r : p["required"])
                        if (r.get<std::string>() == it.key()) req = true;
                out << "- `" << it.key() << "` (" << type << ")"
                    << (req ? ", required" : "") << "\n";
            }
            out << "\n";
        }
    }
    return out.str();
}

} // namespace agent
