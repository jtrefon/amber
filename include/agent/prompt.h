
#ifndef AGENT_PROMPT_H
#define AGENT_PROMPT_H

#include <cstddef>
#include <string>

namespace agent {

// Load a Markdown prompt file from disk. Missing files are treated as an empty
// prompt rather than a fatal error so defaults can be supplied inline.
std::string load_prompt(const std::string& path);

// Read a text file with a cap: the last complete line inside `cap` survives and
// `truncated` reports whether anything was dropped. A block the agent acts on
// must not be cut mid-line -- a marker mid-word reads as corruption -- and a
// silent cut would be worse than either.
std::string read_capped_file(const std::string& path, std::size_t cap, bool& truncated);

// Locate an optional prompt file: next to the binary first (which is where an
// install puts it, and where the benchmark runner finds it despite a different
// CWD), then CWD-relative (a dev run from the repo root). Returns the resolved
// path, or the name itself so the caller's load fails visibly.
std::string optional_prompt_path(const std::string& name);

// The same two-step resolution, then load: empty when the file is absent.
std::string load_optional_prompt(const std::string& name);

// Where a tool's own documentation sits in the tools prompt. Declared here so
// the order is a decision, not an accident of registration: the numbers mirror
// the order the sections are read in, search first.
// Where the project's own instructions sit: before any tool documentation,
// because "how this project is built and what is out of bounds" is what every
// turn needs, and a tool's reference material can follow it.
namespace prompt_priority {
inline constexpr int kProjectInstructions = 400;
// Amber's own index of the project: derived, user-confirmed facts about how it
// runs and where things live. It sits next to the instructions because the two
// answer the same question -- what is true of this project -- from different
// sources, and the project's declared file outranks the derived one.
inline constexpr int kProjectIndex = 410;
// Domain blocks (an activity's prose) sit just after the project's own
// instructions: the project's rules come first, and the activity's follow.
// Identity, then practice, then the discipline that practice draws on.
inline constexpr int kDomainIdentity = 420;
inline constexpr int kDomainWorkflow = 430;
inline constexpr int kDomainDiscipline = 440;
// The domain's library index: one trigger line per entry the project's own tags
// select. A per-turn block (placement Head), so it sits after the system prefix
// and a change to it costs the cache only from its own position.
inline constexpr int kDomainLibrary = 450;
} // namespace prompt_priority

namespace tool_doc_priority {
inline constexpr int kSearch = 500;
inline constexpr int kRead = 510;
inline constexpr int kWrite = 520;
inline constexpr int kBash = 530;
inline constexpr int kProcess = 540;
inline constexpr int kPlan = 550;
inline constexpr int kSkills = 560;
inline constexpr int kMcp = 570;
} // namespace tool_doc_priority

// Render the tool advertising section: a Markdown block listing each tool's
// name, description, and parameter summary. UIs may show this; the library
// also folds it into the system prompt when no explicit tools prompt exists.
std::string render_tools_markdown(const class ToolRegistry& registry);

} // namespace agent

#endif // AGENT_PROMPT_H
