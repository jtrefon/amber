#include "coding_plugin.h"

#include "agent/extensions.h"
#include "agent/prompt.h"
#include "coding_library.h"
#include "library_lookup_tool.h"

#include <filesystem>

namespace agent::plugins {

namespace {

std::string entry_text(const std::string& name) {
    for (const auto& entry : library_entries()) {
        if (entry.name == name)
            return read_library_text(entry.path);
        if (!entry.topic)
            continue;
        for (const auto& child :
             topic_entries(std::filesystem::path(entry.path).parent_path().string()))
            if (child.name == name)
                return read_library_text(child.path);
    }
    return {};
}

// Two blocks, two concerns a reader can take one at a time: how the work moves
// (version control, edits, verification) and how the code is shaped (design
// principles, boundaries, tests). Both are static text, so a session's prompt
// prefix is stable. The profession first: what this session is, before how it is
// done -- core stays agnostic, so a domain that adds a discipline also adds the
// role that carries it.
void add_domain_blocks(std::vector<std::unique_ptr<Capability>>& caps) {
    caps.push_back(std::make_unique<PromptBlockCapability>(
        "coding_identity", prompt_priority::kDomainIdentity,
        [] { return load_optional_prompt("prompts/domains/coding/identity.md"); },
        PromptPlacement::System));
    caps.push_back(std::make_unique<PromptBlockCapability>(
        "coding_workflow", prompt_priority::kDomainWorkflow,
        [] { return load_optional_prompt("prompts/domains/coding/git.md"); },
        PromptPlacement::System));
    caps.push_back(std::make_unique<PromptBlockCapability>(
        "coding_engineering", prompt_priority::kDomainDiscipline,
        [] { return load_optional_prompt("prompts/domains/coding/engineering.md"); },
        PromptPlacement::System));
    // The library's working index: one trigger line per entry this project's own
    // tags select, plus the craft layer, which is indexed for every project.
    // Head placement, because it is derived from state that can change during a
    // session and belongs after the prefix the server caches -- and because the
    // domain that owns the library is the thing that switches it off.
    caps.push_back(std::make_unique<PromptBlockCapability>(
        "coding_library_index", prompt_priority::kDomainLibrary,
        [] { return render_working_index(); }, PromptPlacement::Head));
}

// The library's inventory in the skill-discovery slot. The model looks for
// "skills" there, and the library is not a skill body: this is the one line that
// tells it the library exists and how to reach it. The body is the vocabulary,
// not the entries.
std::unique_ptr<Capability> make_library_skill() {
    SystemSkill skill;
    skill.name = "coding-library";
    skill.description =
        "Amber's coding library: OOP and SOLID, design patterns, architecture, language depth — "
        "query it with library_lookup";
    skill.body = [] { return render_library_skill_body(); };
    return std::make_unique<SkillCapability>(std::move(skill));
}

// The library's retrieval tool travels with the domain that owns the library:
// switching the domain off takes the index line and the way to follow it in the
// same action.
std::unique_ptr<Capability> make_library_tool() {
    return std::make_unique<ToolCapability>(
        "coding_library_tool", [](PluginServices&) { return wrap_library_lookup(); },
        ToolCapability::Meta{{{"library_lookup", {"reading", ToolRole::Other}}}});
}

std::unique_ptr<Capability> make_library_command() {
    CommandSpec library;
    library.name = "library";
    library.help = "The coding library: the vocabulary, the entries, the current selection";
    library.man =
        "Usage: /library list — the vocabulary and every entry. /library show <name> — print one "
        "entry. /library selection — the trigger lines the working index renders this session.";
    library.subtree = json::parse(R"({
        "list": {"help": "List the vocabulary and every entry", "man": "Usage: /library list"},
        "show": {"help": "Print one entry", "man": "Usage: /library show <name>"},
        "selection": {"help": "Show the working index", "man": "Usage: /library selection"}
    })");
    library.handlers["list"] = [](const std::string&) { return render_library_list(); };
    library.handlers["show"] = [](const std::string& arg) {
        if (arg.empty())
            return std::string("Usage: /library show <name>");
        const std::string text = entry_text(arg);
        return text.empty() ? "no library entry named '" + arg + "'" : text;
    };
    library.handlers["selection"] = [](const std::string&) { return render_working_index(); };
    return std::make_unique<CommandCapability>(std::move(library));
}

} // namespace

std::vector<std::unique_ptr<Capability>> CodingPlugin::capabilities() {
    std::vector<std::unique_ptr<Capability>> caps;
    add_domain_blocks(caps);
    caps.push_back(make_library_skill());
    caps.push_back(make_library_tool());
    caps.push_back(make_library_command());
    return caps;
}

} // namespace agent::plugins
