
#ifndef AGENT_POLICY_ENGINE_H
#define AGENT_POLICY_ENGINE_H

#include <string>

#include <nlohmann/json.hpp>

#include "agent/config.h"

namespace agent {

class PolicyStore;
class Tool;
struct AgentHooks;

using json = nlohmann::json;

// What the approval gate should do with a tool call.
enum class Verdict : std::uint8_t {
    Allow,       // run without a dialog
    DenySilent,  // block without a dialog (read mode / stored always-deny)
    Prompt       // ask the host via AgentHooks::on_approval
};

struct Decision {
    Verdict v = Verdict::Allow;
    std::string scope_id;  // resource scope the decision (and grants) key on
};

// Decide whether a tool call needs a dialog. Pure: reads the mode, the tool's
// own approval contract, the shell classifier (for bash), and the policy
// store's persisted/session rules. Never calls the host hook — the caller
// prompts only when the verdict is Prompt and applies the dialog result back
// through PolicyStore::set_rule/grant_session/record_choice.
//
// Semantics:
//   YOLO mode                        → Allow (no dialog ever)
//   READ mode                        → DenySilent for any non-read-only tool
//   policy_approval off              → Allow
//   tool doesn't require approval    → Allow
//   stored AlwaysAllow scope rule    → Allow
//   stored AlwaysDeny scope rule     → DenySilent
//   session grant for the scope      → Allow
//   benign in-workspace write        → Allow (WRITE mode)
//   otherwise                        → Prompt (destructive / outside / unknown)
Decision decide_approval(const Config& cfg, const Tool& tool, const json& args,
                         PolicyStore& policy);

} // namespace agent

#endif // AGENT_POLICY_ENGINE_H
