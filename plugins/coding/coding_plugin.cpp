
#include "coding_plugin.h"

#include "agent/extensions.h"
#include "agent/prompt.h"

namespace agent::plugins {

std::vector<std::unique_ptr<Capability>> CodingPlugin::capabilities() {
    std::vector<std::unique_ptr<Capability>> caps;
    // Two blocks, two concerns a reader can take one at a time: how the work
    // moves (version control, edits, verification) and how the code is shaped
    // (design principles, boundaries, tests). Both are static text, so a
    // session's prompt prefix is stable.
    // The profession first: what this session is, before how it is done. Core
    // stays agnostic, so a domain that adds a discipline also adds the role
    // that carries it.
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
    return caps;
}

} // namespace agent::plugins
