
#ifndef AGENT_POLICY_H
#define AGENT_POLICY_H

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
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

// A stored approval rule keyed by a scope id. Scope ids are:
//   "bash:rm"              — a destructive command pattern (see
//                            shell_classify.h), e.g. "bash:rm", "bash:git reset"
//   "bash:*"               — any unclassifiable / path-qualified bash command
//   "outside:/abs/dir"     — writes (redirects/paths) outside the workspace
//   "<tool>"               — a whole tool (e.g. "write", "process_start");
//                            legacy rules persisted with args_pattern == ""
//                            migrate to this bare-tool scope on load.
// `args_pattern` holds the pattern portion ("rm", "git reset", "") and is
// matched by prefix against the command words at the gate.
struct PolicyRule {
    std::string tool;
    std::string args_pattern;
    PolicyLevel level = PolicyLevel::Ask;
    PolicyLevel last_choice = PolicyLevel::AllowOnce;
    int count = 0;
    std::string created;
    std::string last_used;
};

// Human-readable scope for menus/completions: "bash:rm" -> "bash.rm",
// "outside:/etc" -> "outside./etc", bare "write" stays "write".
inline std::string scope_display(const PolicyRule& r) {
    std::string s = r.args_pattern.empty() ? r.tool
                                           : r.tool + "." + r.args_pattern;
    std::replace(s.begin(), s.end(), ':', '.');
    return s;
}

class PolicyStore {
public:
    void load(const std::string& path);
    void save(const std::string& path) const;

    // Look up the rule whose stored scope exactly matches `scope_id`. Legacy
    // whole-tool rules (args_pattern == "") match only a bare-tool scope id.
    const PolicyRule* find(const std::string& scope_id) const;

    void set_rule(const std::string& scope_id, PolicyLevel level);
    void revoke(const std::string& scope_id);
    void record_choice(const std::string& scope_id, PolicyLevel choice);

    bool is_granted_session(const std::string& scope_id) const;
    void grant_session(const std::string& scope_id);
    void clear_session();

    const std::vector<PolicyRule>& rules() const { return rules_; }

    // Initialize: load from path, or seed defaults if file missing.
    void init(const std::string& path);

    static std::vector<PolicyRule> default_harmful_patterns();

private:
    std::vector<PolicyRule> rules_;
    std::set<std::string> session_grants_;

    PolicyRule* mutable_find(const std::string& scope_id);
};

} // namespace agent

#endif // AGENT_POLICY_H
