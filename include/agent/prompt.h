// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AGENT_PROMPT_H
#define AGENT_PROMPT_H

#include <string>

namespace agent {

// Load a Markdown prompt file from disk. Missing files are treated as an empty
// prompt rather than a fatal error so defaults can be supplied inline.
std::string load_prompt(const std::string& path);

// Render the tool advertising section: a Markdown block listing each tool's
// name, description, and parameter summary. UIs may show this; the library
// also folds it into the system prompt when no explicit tools prompt exists.
std::string render_tools_markdown(const class ToolRegistry& registry);

} // namespace agent

#endif // AGENT_PROMPT_H
