
#include "agent/policy_engine.h"
#include "agent/policy.h"
#include "agent/shell_classify.h"
#include "agent/tool.h"
#include "agent/workspace.h"

#include <string>

namespace agent {

Decision decide_approval(const Config& cfg, const Tool& tool, const json& args,
                         PolicyStore& policy) {
    Decision d;

    // YOLO mode bypasses the gate entirely.
    if (cfg.mode == AgentMode::Yolo) return d;

    // Master switch off → run everything (write mode trust).
    if (!cfg.policy_approval) return d;

    // A tool that does not require approval for this invocation runs free
    // (read-only commands; the tool contract is the source of truth).
    if (!tool.requires_approval(args)) return d;

    // READ mode is enforced by the dispatch gate (tool-level is_read_only)
    // before this decision; a gated call that slips through must prompt.
    if (cfg.mode == AgentMode::Read) {
        d.v = Verdict::DenySilent;
        return d;
    }

    std::string cmd;
    if (args.contains("command") && args["command"].is_string())
        cmd = args["command"].get<std::string>();

    // Benign in-workspace writes run free in WRITE mode.
    if (tool.name() == "bash") {
        ShellClass cls = classify_shell(cmd, Workspace::root());
        if (cls.effect == ShellEffect::ReadOnly ||
            cls.effect == ShellEffect::Write)
            return d;  // Allow: no dialog for benign in-workspace work
    }

    // Scope id: bash gets a fine-grained resource scope from the classifier
    // ("bash:rm", "outside:/abs/dir"); other gated tools key on their bare
    // name (write, process_start, ...), which is also the legacy policy key.
    std::string scope = tool.name() == "bash"
                            ? classify_shell(cmd, Workspace::root()).scope_id
                            : tool.name();

    // Stored rules are honored: an always-allow / always-deny for this scope
    // suppresses the dialog entirely (this is the "always" fix).
    if (const PolicyRule* rule = policy.find(scope)) {
        if (rule->level == PolicyLevel::AlwaysAllow) return d;       // Allow
        if (rule->level == PolicyLevel::AlwaysDeny) {
            d.v = Verdict::DenySilent;
            return d;
        }
    }

    // Per-scope session grant (AllowSession earlier this conversation).
    if (policy.is_granted_session(scope)) return d;

    d.v = Verdict::Prompt;
    d.scope_id = scope;
    return d;
}

} // namespace agent
