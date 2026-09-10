
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
void sanitize_tool_schema(json& schema);

// Normalize an assistant message's tool_calls for the wire: drop entries that
// never received a function name (e.g. "{}" placeholder slots persisted by
// older parsers on sparse-index streams — a strict gateway rejects them with
// a type-discriminator 400) and default a missing/empty `type` to "function".
// Returns the sanitized copy; the caller's history is untouched.
json sanitize_tool_calls(const json& calls);

} // namespace agent

#endif // AGENT_DIALECT_OPENAI_H
