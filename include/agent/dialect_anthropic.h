
#ifndef AGENT_DIALECT_ANTHROPIC_H
#define AGENT_DIALECT_ANTHROPIC_H

// The Anthropic Messages API dialect (/v1/messages + /v1/models). Proves the
// dialect seam: adding this protocol touched one new file plus two registry
// rows — no transport, agent-loop, or UI changes.

#include "agent/dialect.h"

namespace agent {

std::unique_ptr<Dialect> make_anthropic_dialect();

} // namespace agent

#endif // AGENT_DIALECT_ANTHROPIC_H
