
#ifndef AGENT_PROMPT_H
#define AGENT_PROMPT_H

#include <cstddef>
#include <string>

#include "agent/prompt_priorities.h"

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

// Where a tool's own documentation sits in the tools prompt, and where the
// project's own blocks sit relative to it: the tables and their bands live in
// agent/prompt_priorities.h, so the order the model receives has one authority.

// Render the tool advertising section: a Markdown block listing each tool's
// name, description, and parameter summary. UIs may show this; the library
// also folds it into the system prompt when no explicit tools prompt exists.
std::string render_tools_markdown(const class ToolRegistry& registry);

} // namespace agent

#endif // AGENT_PROMPT_H
