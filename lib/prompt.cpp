
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

namespace {

// The fixed preamble: the result envelope (described once because it applies to
// every tool) and the decision-table header.
const char* kToolsPreamble = "# Tools\n\n"
                             "## Result envelope\n\n"
                             "Every tool result uses the exact same form:\n\n"
                             "```\n"
                             "[tool=<name> args=<json> status=<status> meta=<json>]\n"
                             "<content>\n"
                             "[end]\n"
                             "```\n\n"
                             "The envelope is immutable. Only values change. "
                             "`status` is one of: `ok`, `error`, `denied`, `timeout`.\n\n"
                             "| Tool | Use when |\n"
                             "|------|----------|\n";

// One decision-table row: the tool name and the first line of its description.
void render_tool_row(std::ostream& out, const Tool& t) {
    const std::string desc = t.description();
    out << "| `" << t.name() << "` | " << desc.substr(0, desc.find('\n')) << " |\n";
}

// The "- `name` (type, required) — description" list for one tool.
void render_parameter_list(std::ostream& out, const json& p) {
    if (!p.contains("properties"))
        return;
    for (auto it = p["properties"].begin(); it != p["properties"].end(); ++it) {
        const std::string type = it.value().value("type", "any");
        const std::string desc = it.value().value("description", "");
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

} // namespace

std::string render_tools_markdown(const ToolRegistry& registry) {
    std::stringstream out;
    out << kToolsPreamble;
    for (const auto& t : registry.snapshot_tools())
        render_tool_row(out, *t);
    out << "\n";

    // Per-tool reference
    for (const auto& t : registry.snapshot_tools()) {
        out << "## " << t->name() << "\n\n";
        out << t->description() << "\n\n";
        render_parameter_list(out, t->parameters_schema());
    }
    return out.str();
}

} // namespace agent
