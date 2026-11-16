#ifndef AGENT_PROMPT_ASSEMBLER_H
#define AGENT_PROMPT_ASSEMBLER_H

#include <cstddef>
#include <string>
#include <vector>

#include "agent/llm.h"

namespace agent {

class ActivationSink;
class MemoryRetriever;
class PromptRegistry;
class SessionBriefStore;
class SkillCatalog;
class ToolRegistry;
struct Config;

// A prompt block waiting to be placed, ordered by (priority, insertion).
struct PromptBlock {
    int priority = 0;
    std::size_t seq = 0;
    std::string text;
};

// The per-agent state a request prompt is assembled from. The assembler holds
// none of it: the agent remains the owner, and one request reads these
// sources. Null pointers mean that source contributes nothing.
struct PromptSources {
    const ToolRegistry* registry = nullptr;
    const MemoryRetriever* memory = nullptr;
    const SkillCatalog* skills = nullptr;
    const ActivationSink* activated = nullptr;
    const SessionBriefStore* brief = nullptr;
    PromptRegistry* prompts = nullptr;
};

// The stable-prefix inputs. The environment card and workspace note are
// session-fixed: the agent probes them once, so assembly never shells out.
struct SystemPromptSources {
    const Config* cfg = nullptr;
    const ToolRegistry* registry = nullptr;
    const PromptRegistry* prompts = nullptr;
    const std::string* environment_card = nullptr;
    const std::string* workspace_note = nullptr;
    bool subagent = false;
};

// Assemble the two prompt surfaces: the stable system prefix and the injected
// blocks for one request. Stateless; assembly is deterministic for identical
// sources, and one instance can serve every agent.
class PromptAssembler {
public:
    // The complete system message: base prompt, environment card, tool prose,
    // System plugin blocks, mode paragraph, workspace note, git prose, worker
    // boundary. Throws when the configured system prompt is missing or empty.
    static std::string system_prompt(const SystemPromptSources& sources);

    // The blocks injection would add, in request order.
    static void collect_blocks(const PromptSources& sources, std::vector<PromptBlock>& head,
                               std::vector<PromptBlock>& tail);

    // Insert head blocks after the system prompt and append tail blocks to the
    // conversation. The sealed Context is never touched — this is the copy.
    static void inject_blocks(const PromptSources& sources, std::vector<Message>& prompt_copy);

    // Sort by (priority, insertion). Ties keep insertion order.
    static void order_blocks(std::vector<PromptBlock>& blocks);
};

} // namespace agent

#endif // AGENT_PROMPT_ASSEMBLER_H
