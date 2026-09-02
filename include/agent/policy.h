
#ifndef AGENT_POLICY_H
#define AGENT_POLICY_H

#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace agent {

using json = nlohmann::json;

enum class PolicyLevel : std::uint8_t {
    Ask,           // prompt every time (default / no stored rule)
    AllowOnce,     // just this call
    AllowSession,  // rest of this conversation
    AlwaysAllow,   // persisted — never ask again
    AlwaysDeny     // persisted — always block
};

inline const char* policy_level_name(PolicyLevel l) {
    switch (l) {
        case PolicyLevel::Ask:           return "ask";
        case PolicyLevel::AllowOnce:     return "allow_once";
        case PolicyLevel::AllowSession:  return "allow_session";
        case PolicyLevel::AlwaysAllow:   return "allow";
        case PolicyLevel::AlwaysDeny:    return "deny";
    }
    return "ask";
}

inline PolicyLevel policy_level_from_name(const std::string& n) {
    if (n == "allow" || n == "always_allow") return PolicyLevel::AlwaysAllow;
    if (n == "deny" || n == "always_deny")   return PolicyLevel::AlwaysDeny;
    if (n == "allow_session") return PolicyLevel::AllowSession;
    if (n == "allow_once")    return PolicyLevel::AllowOnce;
    return PolicyLevel::Ask;
}

struct PolicyRule {
    std::string tool;
    std::string args_pattern;
    PolicyLevel level = PolicyLevel::Ask;
    PolicyLevel last_choice = PolicyLevel::AllowOnce;
    int count = 0;
    std::string created;
    std::string last_used;
};

class PolicyStore {
public:
    void load(const std::string& path);
    void save(const std::string& path) const;

    const PolicyRule* find(const std::string& tool) const;

    void set_rule(const std::string& tool, PolicyLevel level);
    void revoke(const std::string& tool);
    void record_choice(const std::string& tool, PolicyLevel choice);

    bool is_granted_session(const std::string& tool) const;
    void grant_session(const std::string& tool);
    void clear_session();

    PolicyLevel last_choice(const std::string& tool) const;
    const std::vector<PolicyRule>& rules() const { return rules_; }

    // Initialize: load from path, or seed defaults if file missing.
    void init(const std::string& path);

    static std::vector<PolicyRule> default_harmful_patterns();

private:
    std::vector<PolicyRule> rules_;
    std::set<std::string> session_grants_;
    std::unordered_map<std::string, PolicyLevel> last_choices_;

    PolicyRule* mutable_find(const std::string& tool);
};

// Approval result from the host dialog, extended with persistent levels.
enum class ApprovalResult : std::uint8_t {
    Deny,           // reject this invocation only
    AllowOnce,      // permit just this one call
    AllowSession,   // permit for rest of conversation
    AlwaysAllow,    // persist — never ask again for this tool
    AlwaysDeny      // persist — always block this tool
};

} // namespace agent

#endif // AGENT_POLICY_H
