#ifndef AGENT_TOOLSET_AUDIT_H
#define AGENT_TOOLSET_AUDIT_H

// The toolset audit (tools-domain spec §5.2).
//
// Switching a plugin off changes what the model can do, and the failure is
// silent: the harness keeps working, just worse. The audit names the two
// shapes that failure takes.
//
// It is a *warning* system, never a gate. Two tools competing for one job is a
// description problem the bench can measure, not a permission the harness
// should withhold - and the two-search-implementations comparison this domain
// exists to enable is exactly that shape (spec §5.2).

#include "agent/registry.h"

#include <string>
#include <vector>

namespace agent {

struct AuditFinding {
    enum class Kind : std::uint8_t {
        // The enabled set can no longer do something amber assumes it can.
        Deficiency,
        // Two enabled tools claim the same job and describe it the same way,
        // which is what actually confuses a model choosing between them.
        Ambiguity,
    };

    Kind kind = Kind::Deficiency;
    std::string message;   // one line, naming the tools involved
};

std::string to_string(AuditFinding::Kind kind);

// Audit the enabled set. Empty means the set is complete enough and its tools
// are distinguishable.
std::vector<AuditFinding> audit_toolset(const ToolRegistry& registry);

// The roles amber assumes the enabled set can perform. Read is the floor: a
// harness that cannot bring back content cannot answer a question about the
// workspace at all. Adding to this list is a statement about what amber is,
// not a preference - which is why it is short.
std::vector<ToolRole> required_tool_roles();

} // namespace agent

#endif // AGENT_TOOLSET_AUDIT_H
