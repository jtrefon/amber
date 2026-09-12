#ifndef AGENT_TOOL_PROMPT_H
#define AGENT_TOOL_PROMPT_H

#include "agent/prompt.h" // tool_doc_priority

#include <memory>
#include <string>

namespace agent {

class Capability;

// A tool's own documentation, contributed as a System prompt block: it lands
// in the system prompt beside the schema it describes, so the two cannot
// disagree. A tool that is switched off takes its prose with it.
//
// The text lives in prompts/tools/<file>, Markdown like every other prompt, so
// editing it changes behaviour without a rebuild. A missing file renders empty
// and the block is skipped.
std::unique_ptr<Capability> make_tool_doc_capability(const std::string& id, int priority,
                                                     const std::string& file);

} // namespace agent

#endif // AGENT_TOOL_PROMPT_H
