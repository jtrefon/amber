
#ifndef AGENT_DIALECT_OPENAI_H
#define AGENT_DIALECT_OPENAI_H

// The OpenAI-compatible dialect (/chat/completions + /models). Also the
// fallback for unknown flavors. Exposes the wire-hygiene helpers that the
// transport and tests pin directly.

#include "agent/dialect.h"

namespace agent {

std::unique_ptr<Dialect> make_openai_dialect();

// Repair a tool parameters_schema in place so the server's grammar builder
// never sees null types or arrays without items (llama.cpp 400s with "type
// must be array, but is null"). Applied to every tool before sending.
// Exposed so the wire hygiene is directly pinnable.
void sanitize_tool_schema(json& schema);

} // namespace agent

#endif // AGENT_DIALECT_OPENAI_H
