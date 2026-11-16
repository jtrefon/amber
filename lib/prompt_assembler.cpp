#include "agent/prompt_assembler.h"

#include "agent/activation_sink.h"
#include "agent/config.h"
#include "agent/experience.h"
#include "agent/extensions.h"
#include "agent/plugin.h"
#include "agent/prompt.h"
#include "agent/prompt_priorities.h"
#include "agent/session_brief.h"
#include "agent/skill_catalog.h"
#include "agent/subagent.h"
#include "agent/tools.h"

#include <algorithm>
#include <stdexcept>

namespace agent {

namespace {

// Append a Markdown section to the system prompt. Files carry a trailing
// newline and the separator is two of them, so the section is trimmed before it
// is joined - otherwise every boundary gains a blank line and the prompt drifts
// from what the files say.
void append_section(std::string& system, const std::string& text) {
    const std::size_t end = text.find_last_not_of('\n');
    if (end == std::string::npos)
        return;
    system += "\n\n";
    system.append(text, 0, end + 1);
}

// Append a section only when it has content; sections are separated by a blank
// line.
void append_optional_section(std::string& system, const std::string& text) {
    if (!text.empty())
        system += "\n\n" + text;
}

// The mode paragraph appended to every system prompt.
std::string mode_prompt(AgentMode mode) {
    switch (mode) {
    case AgentMode::Read:
        return "\n\nYou are in READ mode: you can explore and answer "
               "questions using read-only tools (search, grep, read). "
               "Writing files or running shell commands is not available "
               "here, so prefer read-only investigation and report back.";
    case AgentMode::Write:
        return "\n\nYou are in WRITE mode: you work inside the workspace, "
               "reading, editing, creating files and running commands "
               "freely. Destructive or system-wide commands (rm, dd, git "
               "reset, sudo, package installs, ...) pause for the user's "
               "approval, as does writing outside the workspace (once per "
               "target folder). A command request the user grants runs "
               "silently for the rest of the session or permanently.";
    case AgentMode::Yolo:
        return "\n\nYou are in YOLO mode: every tool runs immediately with "
               "no approval gate. The user trusts you completely, so keep "
               "the same careful, deliberate working style.";
    }
    return "";
}

bool block_before(const PromptBlock& a, const PromptBlock& b) {
    return a.priority != b.priority ? a.priority < b.priority : a.seq < b.seq;
}

} // namespace

std::string PromptAssembler::system_prompt(const SystemPromptSources& sources) {
    if (!sources.cfg || !sources.registry)
        throw std::invalid_argument("prompt assembly needs a config and a tool registry");
    const Config& cfg = *sources.cfg;

    std::string system = load_prompt(cfg.system_prompt_path);
    if (system.empty())
        throw std::runtime_error("system prompt file not found or empty: " +
                                 cfg.system_prompt_path);
    // Environment card: OS, user, resources, available tools. Session-fixed by
    // construction, because re-probing every turn would both shell out per
    // request and rewrite the cached prefix when the answer moved.
    if (sources.environment_card)
        append_optional_section(system, *sources.environment_card);

    if (!cfg.tools_prompt_path.empty()) {
        std::string tools = load_prompt(cfg.tools_prompt_path);
        if (tools.empty())
            throw std::runtime_error("tools prompt file not found or empty: " +
                                     cfg.tools_prompt_path);
        append_section(system, tools);
    } else if (!sources.registry->empty()) {
        append_section(system, render_tools_markdown(*sources.registry));
    }
    // Tool documentation contributed as System blocks lands here, immediately
    // after the tools prompt it belongs to. This is what keeps a tool's schema
    // and its prose in agreement: disabling a tool plugin removes both.
    if (sources.prompts) {
        for (auto& text : sources.prompts->render_all(PromptPlacement::System))
            append_section(system, text);
    }
    // Plugins: enabled plugin tools (registered as plugin_<id>_<name>) get
    // their own reference section so the agent knows they exist and how to use
    // them without touching the static tools.md.
    append_optional_section(system, plugin_tools_advertisement(*sources.registry));
    system += mode_prompt(cfg.mode);

    // The workspace itself is part of the environment: a detached worktree
    // needs a branch before its first commit, and nothing else in the prompt
    // knows that.
    if (sources.workspace_note)
        append_optional_section(system, *sources.workspace_note);

    // An explicit git prompt, when the user configured one. The standard git
    // prose lives in the coding domain plugin, so a session that is not
    // software work carries none of it.
    if (!cfg.git_prompt_path.empty())
        append_optional_section(system, load_prompt(cfg.git_prompt_path));
    // A worker runs this same assembly plus the role boundary: rendered here,
    // not composed by the delegate tool, so the prompt rebuild cannot lose it.
    if (sources.subagent)
        append_section(system, worker_directive());
    return system;
}

// Discovery metadata is cheap and constant; skill bodies load only when the
// model asks for one with read_skill, so they trail the conversation.
namespace {

void collect_skill_blocks(const PromptSources& sources, std::vector<PromptBlock>& head,
                          std::vector<PromptBlock>& tail, std::size_t& seq) {
    if (!sources.skills)
        return;
    // The discovery block exists to be acted on, and the only actor is
    // read_skill. With that tool switched off (its plugin unwound it), the
    // block would name a tool the registry cannot serve — the same rule the
    // tool prose follows. Activated bodies are not gated: they are content the
    // model has already seen, and dropping them would lose context.
    if (sources.read_skill_available) {
        const auto discovery = sources.skills->discovery_block();
        if (!discovery.empty()) {
            std::string text = "Available skills (activate with read_skill):\n";
            for (const auto& line : discovery)
                text += line + "\n";
            head.push_back({prompt_priority::kSkillDiscovery, seq++, std::move(text)});
        }
    }
    if (!sources.activated)
        return;
    for (const auto& act : sources.activated->snapshot())
        tail.push_back({prompt_priority::kActivatedSkills, seq++,
                        "[activated skill: " + act.name + "]\n" + act.body});
}

} // namespace

void PromptAssembler::collect_blocks(const PromptSources& sources, std::vector<PromptBlock>& head,
                                     std::vector<PromptBlock>& tail) {
    std::size_t seq = 0;

    // The learned-memory block is the [system][memory] prefix, so it is a
    // function of the store alone. The retriever caches it until the store
    // version moves; selection never follows the current question.
    if (sources.memory) {
        std::string memory = sources.memory->build_block();
        if (!memory.empty())
            head.push_back({prompt_priority::kMemory, seq++, std::move(memory)});
    }

    collect_skill_blocks(sources, head, tail, seq);

    // The brief lives in its store, not the context, so compression cannot lose
    // it; it sits at the tail because it changes more often than the rest.
    if (sources.brief) {
        std::string rendered = sources.brief->render();
        if (!rendered.empty())
            tail.push_back({prompt_priority::kSessionBrief, seq++, std::move(rendered)});
    }

    // Plugin blocks come from the shared registry, which has already ordered
    // them by (priority, registration), and each declares its own placement:
    // Head blocks read as instructions for this request, Tail blocks follow the
    // conversation. System blocks are not here at all - they are part of the
    // system prompt itself (system_prompt), because that is the stable prefix
    // and rewriting it per turn would cost the cache.
    if (sources.prompts) {
        for (auto& text : sources.prompts->render_all(PromptPlacement::Head))
            head.push_back({prompt_priority::kPluginBlock, seq++, std::move(text)});
        for (auto& text : sources.prompts->render_all(PromptPlacement::Tail))
            tail.push_back({prompt_priority::kPluginBlock, seq++, std::move(text)});
    }
}

void PromptAssembler::order_blocks(std::vector<PromptBlock>& blocks) {
    std::sort(blocks.begin(), blocks.end(), block_before);
}

// Head blocks go immediately after the system prompt, so they read as
// instructions for this request; tail blocks follow the conversation. The tail
// rides the user channel (with the harness flag) rather than the system one:
// every dialect hoists system messages into the leading block, and a cacheable
// prefix cannot contain content that changes between turns. On the wire the
// tail therefore stays after everything it annotates — a change costs only the
// block itself.
void PromptAssembler::inject_blocks(const PromptSources& sources,
                                    std::vector<Message>& prompt_copy) {
    std::vector<PromptBlock> head;
    std::vector<PromptBlock> tail;
    collect_blocks(sources, head, tail);
    order_blocks(head);
    order_blocks(tail);

    std::size_t pos = 0;
    for (std::size_t i = 0; i < prompt_copy.size(); ++i) {
        if (prompt_copy[i].role == "system") {
            pos = i + 1;
            break;
        }
    }
    for (auto& block : head) {
        Message msg;
        msg.role = "system";
        msg.content = std::move(block.text);
        prompt_copy.insert(prompt_copy.begin() + static_cast<std::ptrdiff_t>(pos), std::move(msg));
        ++pos;
    }
    for (auto& block : tail) {
        Message msg;
        msg.role = "user";
        msg.harness = true;
        msg.content = std::move(block.text);
        prompt_copy.push_back(std::move(msg));
    }
}

} // namespace agent
