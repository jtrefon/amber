
#ifndef AGENT_PROMPT_H
#define AGENT_PROMPT_H

#include <string>

namespace agent {

// Load a Markdown prompt file from disk. Missing files are treated as an empty
// prompt rather than a fatal error so defaults can be supplied inline.
std::string load_prompt(const std::string& path);

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
namespace tool_doc_priority {
inline constexpr int kSearch = 500;
inline constexpr int kRead = 510;
inline constexpr int kWrite = 520;
inline constexpr int kBash = 530;
inline constexpr int kProcess = 540;
} // namespace tool_doc_priority

// Render the tool advertising section: a Markdown block listing each tool's
// name, description, and parameter summary. UIs may show this; the library
// also folds it into the system prompt when no explicit tools prompt exists.
std::string render_tools_markdown(const class ToolRegistry& registry);

} // namespace agent

#endif // AGENT_PROMPT_H
