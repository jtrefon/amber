#include "agent/tool_prompt.h"

#include "agent/extensions.h"
#include "agent/prompt.h"

namespace agent {

std::unique_ptr<Capability> make_tool_doc_capability(const std::string& id, int priority,
                                                     const std::string& file) {
    return std::make_unique<PromptBlockCapability>(
        id, priority, [file] { return load_optional_prompt(file); }, PromptPlacement::System);
}

} // namespace agent
