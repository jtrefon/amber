
#include "agent/skill_commands.h"
#include "agent/config.h"
#include "agent/tools.h"
#include "agent/workspace.h"

#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace agent {

namespace {

const char* scope_name(SkillScope scope) {
    switch (scope) {
        case SkillScope::Project: return "project";
        case SkillScope::Global: return "global";
        case SkillScope::Interop: return "interop";
    }
    return "unknown";
}

const char* origin_name(SkillOrigin origin) {
    return origin == SkillOrigin::Learned ? "learned" : "authored";
}

std::string scope_dir(const std::string& scope, std::string& error) {
    if (scope == "project") return Workspace::local_dir() + "/skills";
    if (scope == "global") return global_config_dir() + "/skills";
    error = "invalid scope '" + scope + "' (use 'project' or 'global')";
    return "";
}

} // namespace

std::vector<std::string> skill_show_lines(const SkillCatalog& catalog) {
    std::vector<std::string> lines;
    for (const auto& e : catalog.entries()) {
        lines.push_back(std::string(scope_name(e.scope)) + " \u00b7 " +
                        origin_name(e.origin) + " \u00b7 " + e.name +
                        " \u00b7 " + e.state);
    }
    for (const auto& kv : catalog.overrides()) {
        if (kv.second.state == "disable" || kv.second.state == "block") {
            bool listed = false;
            for (const auto& e : catalog.entries())
                if (e.name == kv.first) listed = true;
            if (!listed) {
                lines.push_back(std::string("project \u00b7 authored \u00b7 ") +
                                kv.first + " \u00b7 " + kv.second.state + "d");
            }
        }
    }
    return lines;
}

std::string skill_create(SkillCatalog& catalog, const std::string& name,
                         const std::string& description,
                         const std::string& body,
                         const std::string& scope) {
    return author_skill(catalog, name, description, body, scope);
}

std::string skill_delete(SkillCatalog& catalog, const std::string& name,
                         const std::string& scope) {
    std::string error;
    std::string dir = scope_dir(scope, error);
    if (dir.empty()) return error;
    bool found = false;
    for (const auto& e : catalog.entries()) {
        if (e.name != name) continue;
        if (scope == "project" && e.scope != SkillScope::Project) continue;
        if (scope == "global" && e.scope != SkillScope::Global) continue;
        found = true;
        break;
    }
    if (!found) return "no skill named '" + name + "' in " + scope + " scope";
    std::error_code ec;
    fs::remove_all(fs::path(dir) / name, ec);
    if (ec) return "cannot remove " + dir + "/" + name;
    catalog.refresh();
    return "";
}

std::string skill_export(SkillCatalog& catalog, const std::string& name) {
    const SkillEntry* learned = nullptr;
    for (const auto& e : catalog.entries()) {
        if (e.name == name && e.origin == SkillOrigin::Learned) {
            learned = &e;
            break;
        }
    }
    if (!learned)
        return "no learned skill named '" + name + "'";
    std::string body = learned->meta.body;
    if (body.empty()) body = name;
    std::string description = learned->meta.description.empty()
        ? name : learned->meta.description;
    return author_skill(catalog, name, description, body, "global");
}

std::string skill_set_override(SkillCatalog& catalog, const std::string& name,
                               const std::string& state) {
    if (state != "enable" && state != "disable" && state != "block")
        return "invalid state '" + state + "' (use enable, disable, block)";
    std::string note;
    if (state == "block") {
        for (const auto& e : catalog.entries()) {
            if (e.name != name) continue;
            std::string author = e.meta.metadata.value("author", "");
            if (!author.empty())
                note = "blocked by user (author: " + author + ")";
            break;
        }
    }
    if (!catalog.apply_override(name, state, note))
        return "could not persist override for '" + name + "'";
    catalog.refresh();
    return "";
}

} // namespace agent
