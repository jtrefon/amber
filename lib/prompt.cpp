
#include "agent/prompt.h"

#include "agent/data_path.h"
#include "agent/registry.h"

#include <fstream>
#include <sstream>

namespace agent {

std::string load_prompt(const std::string& path) {
    if (path.empty())
        return "";
    std::ifstream in(path);
    if (!in)
        return "";
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string optional_prompt_path(const std::string& name) {
    const std::string dir = exe_dir();
    if (!dir.empty()) {
        const std::string p = dir + "/" + name;
        if (file_exists(p))
            return p;
    }
    return name;
}

std::string load_optional_prompt(const std::string& name) {
    return load_prompt(optional_prompt_path(name));
}

std::string render_tools_markdown(const ToolRegistry& registry) {
    std::stringstream out;
    out << "# Tools\n\n"

        // Envelope — described once, applies to every tool
        << "## Result envelope\n\n"
        << "Every tool result uses the exact same form:\n\n"
        << "```\n"
        << "[tool=<name> args=<json> status=<status> meta=<json>]\n"
        << "<content>\n"
        << "[end]\n"
        << "```\n\n"
        << "The envelope is immutable. Only values change. "
        << "`status` is one of: `ok`, `error`, `denied`, `timeout`.\n\n"

        // Decision table
        << "| Tool | Use when |\n"
           "|------|----------|\n";
    for (const auto& t : registry.snapshot_tools())
        out << "| `" << t->name() << "` | "
            << t->description().substr(0, t->description().find('\n')) << " |\n";
    out << "\n";

    // Per-tool reference
    for (const auto& t : registry.snapshot_tools()) {
        out << "## " << t->name() << "\n\n";
        out << t->description() << "\n\n";
        json p = t->parameters_schema();
        if (p.contains("properties")) {
            for (auto it = p["properties"].begin(); it != p["properties"].end(); ++it) {
                std::string type = it.value().value("type", "any");
                std::string desc = it.value().value("description", "");
                bool req = false;
                if (p.contains("required"))
                    for (const auto& r : p["required"])
                        if (r.get<std::string>() == it.key())
                            req = true;
                out << "- `" << it.key() << "` (" << type << (req ? ", required" : "") << ")";
                if (!desc.empty())
                    out << " — " << desc;
                out << "\n";
            }
            out << "\n";
        }
    }
    return out.str();
}

} // namespace agent
