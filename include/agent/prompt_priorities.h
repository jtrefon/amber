#ifndef AGENT_PROMPT_PRIORITIES_H
#define AGENT_PROMPT_PRIORITIES_H

#include <cstddef>

// Every injected-block priority, in one table, banded by owner.
//
// Two sources contribute blocks and they keep their ownership split: per-agent
// blocks (memory, skills, brief) are assembled by the agent that owns their
// state, while registry blocks are app-wide and shared by every window, so
// moving one into the other's place would leak a window's state into another's
// prompt (docs/spec/agent-loop/prompt-assembly.md, "Why core blocks are not
// registry entries"). What is shared here is the ordering vocabulary: one file
// and one set of bands, so "what runs before what" is a single lookup instead
// of a two-header reconstruction.
//
// Bands ascend, and a block's band is its owner:
//
//   [100, 300)   agent head   per-agent, placed before the conversation
//   [300, 500)   project      the project's own facts, then the activity's prose
//   [500, 900)   tool docs    one section per tool in the tools prompt
//   [900, 1000)  agent tail   per-agent, appended after the conversation
//   [1000,1100)  plugin       one bucket for every assembled plugin block
//
// The head/tail split is not cosmetic: head blocks read as instructions for
// this request, tail blocks are trailing context that changes more often. Each
// tail block sits after the conversation, so a change to it costs the server's
// KV cache only from its own position (invariant 4 in the prompt-assembly
// spec).
namespace agent {

enum class PromptBand : unsigned char { AgentHead, Project, ToolDoc, AgentTail, Plugin };

// The band a priority must fall inside, half-open. One place to read, one place
// to change: a new band is a new enumerator here and the uniqueness check below
// keeps the ranges from colliding.
constexpr int prompt_band_begin(PromptBand band) noexcept {
    switch (band) {
    case PromptBand::AgentHead:
        return 100;
    case PromptBand::Project:
        return 300;
    case PromptBand::ToolDoc:
        return 500;
    case PromptBand::AgentTail:
        return 900;
    case PromptBand::Plugin:
        return 1000;
    }
    return 0;
}

constexpr int prompt_band_end(PromptBand band) noexcept {
    switch (band) {
    case PromptBand::AgentHead:
        return prompt_band_begin(PromptBand::Project);
    case PromptBand::Project:
        return prompt_band_begin(PromptBand::ToolDoc);
    case PromptBand::ToolDoc:
        return prompt_band_begin(PromptBand::AgentTail);
    case PromptBand::AgentTail:
        return prompt_band_begin(PromptBand::Plugin);
    case PromptBand::Plugin:
        return 1100;
    }
    return 0;
}

// One row per priority. The constants and the table below are generated from
// these same lists, so a priority cannot exist in one and be missing from the
// other -- which is what makes the collision check cover every future addition
// rather than only the ones somebody remembered to list twice.
//
// The order inside a band is the order the model reads:
//
//   - memory is retrieved for this request and opens the head, so the model
//     reads it before anything that tells it what to do;
//   - skill discovery follows because it points at bodies it may activate;
//   - the project's declared instructions outrank the derived index, and both
//     outrank the activity's prose (identity, then practice, then the
//     discipline that practice draws on, then the library index);
//   - activated skill bodies and the session brief are tail blocks, the brief
//     last because it changes most often;
//   - the plugin bucket is a single priority: the PromptRegistry has already
//     ordered its blocks by (priority, registration) before they reach the
//     assembly, and they follow the core blocks.
#define AMBER_AGENT_PROMPT_PRIORITIES(X)                                                           \
    X(kMemory, 100, PromptBand::AgentHead)                                                         \
    X(kSkillDiscovery, 200, PromptBand::AgentHead)                                                 \
    X(kProjectInstructions, 400, PromptBand::Project)                                              \
    X(kProjectIndex, 410, PromptBand::Project)                                                     \
    X(kDomainIdentity, 420, PromptBand::Project)                                                   \
    X(kDomainWorkflow, 430, PromptBand::Project)                                                   \
    X(kDomainDiscipline, 440, PromptBand::Project)                                                 \
    X(kDomainLibrary, 450, PromptBand::Project)                                                    \
    X(kActivatedSkills, 900, PromptBand::AgentTail)                                                \
    X(kSessionBrief, 950, PromptBand::AgentTail)                                                   \
    X(kPluginBlock, 1000, PromptBand::Plugin)

// Where a tool's own documentation sits in the tools prompt: the numbers mirror
// the order the sections are read in, search first.
#define AMBER_TOOL_DOC_PRIORITIES(X)                                                               \
    X(kSearch, 500, PromptBand::ToolDoc)                                                           \
    X(kRead, 510, PromptBand::ToolDoc)                                                             \
    X(kReadResult, 515, PromptBand::ToolDoc)                                                       \
    X(kWrite, 520, PromptBand::ToolDoc)                                                            \
    X(kBash, 530, PromptBand::ToolDoc)                                                             \
    X(kProcess, 540, PromptBand::ToolDoc)                                                          \
    X(kContext, 545, PromptBand::ToolDoc)                                                          \
    X(kPlan, 550, PromptBand::ToolDoc)                                                             \
    X(kSkills, 560, PromptBand::ToolDoc)                                                           \
    X(kMcp, 570, PromptBand::ToolDoc)                                                              \
    X(kRate, 580, PromptBand::ToolDoc)                                                             \
    X(kWatch, 590, PromptBand::ToolDoc)                                                            \
    X(kWeb, 595, PromptBand::ToolDoc)

namespace prompt_priority {

#define AMBER_PROMPT_PRIORITY_DECL(name, value, band) inline constexpr int name = value;
AMBER_AGENT_PROMPT_PRIORITIES(AMBER_PROMPT_PRIORITY_DECL)
#undef AMBER_PROMPT_PRIORITY_DECL

} // namespace prompt_priority

namespace tool_doc_priority {

#define AMBER_TOOL_DOC_PRIORITY_DECL(name, value, band) inline constexpr int name = value;
AMBER_TOOL_DOC_PRIORITIES(AMBER_TOOL_DOC_PRIORITY_DECL)
#undef AMBER_TOOL_DOC_PRIORITY_DECL

} // namespace tool_doc_priority

struct PromptPriorityEntry {
    const char* name;
    int value;
    PromptBand band;
};

inline constexpr PromptPriorityEntry kPromptPriorities[] = {
#define AMBER_PROMPT_PRIORITY_ROW(name, value, band) {#name, value, band},
    AMBER_AGENT_PROMPT_PRIORITIES(AMBER_PROMPT_PRIORITY_ROW)
        AMBER_TOOL_DOC_PRIORITIES(AMBER_PROMPT_PRIORITY_ROW)
#undef AMBER_PROMPT_PRIORITY_ROW
};

// The table is the ordering authority, so its two invariants are checked where
// they cannot be skipped: a priority outside its band, or two priorities that
// collide (which would make their relative order depend on insertion), fails
// the build rather than a single test run.
inline constexpr PromptBand kPromptBands[] = {PromptBand::AgentHead, PromptBand::Project,
                                              PromptBand::ToolDoc, PromptBand::AgentTail,
                                              PromptBand::Plugin};

constexpr std::size_t prompt_priority_count() noexcept {
    return sizeof(kPromptPriorities) / sizeof(kPromptPriorities[0]);
}

constexpr bool prompt_priorities_are_banded() noexcept {
    for (std::size_t i = 0; i < prompt_priority_count(); ++i) {
        const PromptPriorityEntry& entry = kPromptPriorities[i];
        if (entry.value < prompt_band_begin(entry.band) ||
            entry.value >= prompt_band_end(entry.band))
            return false;
        for (std::size_t j = i + 1; j < prompt_priority_count(); ++j) {
            if (kPromptPriorities[j].value == entry.value)
                return false;
        }
    }
    return true;
}

// A band with no row would make the check above vacuous for that whole range,
// so every band must still be represented.
constexpr bool every_prompt_band_has_a_priority() noexcept {
    for (const PromptBand band : kPromptBands) {
        bool found = false;
        for (const PromptPriorityEntry& entry : kPromptPriorities)
            found = found || entry.band == band;
        if (!found)
            return false;
    }
    return true;
}

static_assert(prompt_priorities_are_banded(),
              "every prompt priority must sit inside its band, and no two may share a value");
static_assert(every_prompt_band_has_a_priority(),
              "a prompt band has no priority row, so its range would be unchecked");

} // namespace agent

#endif // AGENT_PROMPT_PRIORITIES_H
