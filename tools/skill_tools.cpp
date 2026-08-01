
#include "agent/config.h"
#include "agent/registry.h"
#include "agent/skill_catalog.h"
#include "agent/tool.h"
#include "agent/tools.h"
#include "agent/workspace.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace agent {

namespace {

std::string scope_dir(const std::string& scope, std::string& error) {
    if (scope == "project") return Workspace::local_dir() + "/skills";
    if (scope == "global") return global_config_dir() + "/skills";
    error = "invalid scope '" + scope + "' (use 'project' or 'global')";
    return "";
}

} // namespace

// Author a SKILL.md into the given scope (project|global). Shared by
// write_skill and /set skills create. Returns an empty string on success or a
// human-readable error message.
std::string author_skill(SkillCatalog& catalog, const std::string& name,
                         const std::string& description,
                         const std::string& body, const std::string& scope) {
    if (!is_kebab_name(name)) {
        return "invalid skill name '" + name +
               "' (lowercase letters, digits, '-' only)";
    }
    if (description.empty())
        return "missing skill description (1-2 sentence trigger guidance)";
    if (body.empty()) return "missing skill body (markdown instructions)";
    std::string error;
    std::string dir = scope_dir(scope, error);
    if (dir.empty()) return error;

    std::error_code ec;
    fs::path target = fs::path(dir) / name;
    fs::create_directories(target, ec);
    if (ec) return "cannot create skill directory: " + target.string();
    {
        std::ofstream f(target / "SKILL.md", std::ios::trunc);
        if (!f) return "cannot write " + (target / "SKILL.md").string();
        f << "---\n"
          << "name: " << name << "\n"
          << "description: " << description << "\n"
          << "---\n"
          << body << "\n";
        if (!f) return "write failed for " + (target / "SKILL.md").string();
    }
    catalog.refresh();
    return "";
}

// read_skill: activate a skill. Loads and caches the SKILL.md body through the
// catalog; the harness appends the body to the next prompt copy.
class ReadSkillTool : public Tool {
public:
    explicit ReadSkillTool(SkillCatalog& catalog) : catalog_(catalog) {}

    std::string name() const noexcept override { return "read_skill"; }

    bool is_read_only() const noexcept override { return true; }

    std::string description() const noexcept override {
        return "Activate a skill by name: loads its SKILL.md body into context. "
               "Call this when a skill listed in the discovery block matches "
               "the current task.";
    }

    json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties",
             {{"name", {{"type", "string"},
                        {"description", "Skill name to activate"}}}}},
            {"required", {"name"}}};
    }

    ToolResult execute(const json& a) const override {
        if (!a.contains("name") || !a["name"].is_string()) {
            ToolResult r;
            r.ok = false;
            r.error = "missing 'name'";
            return r;
        }
        std::string name = a["name"].get<std::string>();
        if (!catalog_.lookup(name)) {
            ToolResult r;
            r.ok = false;
            r.error = "unknown skill: " + name;
            return r;
        }
        auto body = catalog_.activate(name);
        if (!body) {
            ToolResult r;
            r.ok = false;
            r.error = "skill body exceeds skills_body_budget_tokens";
            return r;
        }
        ToolResult r;
        r.output = "Activated skill '" + name + "' (" +
                   std::to_string(body->size()) + " bytes). Body appended to "
                   "context; follow its instructions for the current task.";
        r.meta = {{"name", name}, {"activated", true}};
        return r;
    }

private:
    SkillCatalog& catalog_;
};

// list_skills: return the discovery surface, optionally filtered.
class ListSkillsTool : public Tool {
public:
    explicit ListSkillsTool(SkillCatalog& catalog) : catalog_(catalog) {}

    std::string name() const noexcept override { return "list_skills"; }

    bool is_read_only() const noexcept override { return true; }

    std::string description() const noexcept override {
        return "List available skills (name: description). Optionally filter by "
               "name substring or origin (authored|learned).";
    }

    json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties",
             {{"name", {{"type", "string"},
                        {"description", "Optional substring filter on skill "
                                        "names"}}},
              {"origin", {{"type", "string"},
                          {"description", "Optional origin filter: 'authored' "
                                          "or 'learned'"}}}}},
            {"required", {}}};
    }

    ToolResult execute(const json& a) const override {
        std::string name_filter = a.value("name", "");
        std::string origin_filter = a.value("origin", "");
        if (!origin_filter.empty() && origin_filter != "authored" &&
            origin_filter != "learned") {
            ToolResult r;
            r.ok = false;
            r.error = "invalid origin filter '" + origin_filter +
                      "' (use 'authored' or 'learned')";
            return r;
        }
        std::string out;
        for (const auto& e : catalog_.entries()) {
            if (e.state != "enabled" && e.state != "force-enabled") continue;
            if (!name_filter.empty() &&
                e.name.find(name_filter) == std::string::npos)
                continue;
            if (origin_filter == "authored" &&
                e.origin != SkillOrigin::Authored)
                continue;
            if (origin_filter == "learned" &&
                e.origin != SkillOrigin::Learned)
                continue;
            std::string desc = e.meta.description.empty()
                ? "(no description)" : e.meta.description;
            out += e.name + ": " + desc + "\n";
        }
        if (out.empty()) out = "(no skills match)\n";
        ToolResult r;
        r.output = out;
        return r;
    }

private:
    SkillCatalog& catalog_;
};

// write_skill: author a SKILL.md. Side-effecting and approval-gated; the model
// must only call it on explicit user request.
class WriteSkillTool : public Tool {
public:
    explicit WriteSkillTool(SkillCatalog& catalog) : catalog_(catalog) {}

    std::string name() const noexcept override { return "write_skill"; }

    bool requires_approval(const json&) const noexcept override { return true; }

    std::string description() const noexcept override {
        return "Author a new skill as a SKILL.md file (project or global "
               "scope). ONLY call this when the user explicitly asks to save a "
               "procedure as a skill; never author skills unsolicited.";
    }

    json parameters_schema() const override {
        return {
            {"type", "object"},
            {"properties",
             {{"name", {{"type", "string"},
                        {"description", "kebab-case skill name"}}},
              {"description", {{"type", "string"},
                               {"description", "1-2 sentence trigger guidance"}}},
              {"body", {{"type", "string"},
                        {"description", "Markdown instructions"}}},
              {"scope", {{"type", "string"},
                         {"enum", {"project", "global"}},
                         {"description", "Where to write the skill "
                                         "(default project)"}}}}},
            {"required", {"name", "description", "body"}}};
    }

    std::string summarize(const json& a) const override {
        return "write skill '" + a.value("name", "?") + "' to " +
               a.value("scope", "project") + " scope";
    }

    ToolResult execute(const json& a) const override {
        std::string name = a.value("name", "");
        std::string description = a.value("description", "");
        std::string body = a.value("body", "");
        std::string scope = a.value("scope", "project");
        std::string error = author_skill(catalog_, name, description, body,
                                         scope);
        ToolResult r;
        if (!error.empty()) {
            r.ok = false;
            r.error = error;
            return r;
        }
        r.output = "Skill '" + name + "' written to " + scope +
                   " scope and re-scanned.";
        r.meta = {{"name", name}, {"scope", scope}};
        return r;
    }

private:
    SkillCatalog& catalog_;
};

} // namespace agent

std::unique_ptr<agent::Tool> agent::make_read_skill_tool(
    SkillCatalog& catalog) {
    return std::make_unique<agent::ReadSkillTool>(catalog);
}

std::unique_ptr<agent::Tool> agent::make_list_skills_tool(
    SkillCatalog& catalog) {
    return std::make_unique<agent::ListSkillsTool>(catalog);
}

std::unique_ptr<agent::Tool> agent::make_write_skill_tool(
    SkillCatalog& catalog) {
    return std::make_unique<agent::WriteSkillTool>(catalog);
}

void agent::register_skill_tools(ToolRegistry& reg, SkillCatalog& catalog) {
    reg.register_tool(make_read_skill_tool(catalog));
    reg.register_tool(make_list_skills_tool(catalog));
    reg.register_tool(make_write_skill_tool(catalog));
}
