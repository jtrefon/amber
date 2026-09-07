
#include "agent/policy.h"
#include "agent/shell_classify.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace agent {

namespace {

// Parse a scope id into (tool, pattern). Scopes look like "bash:rm",
// "bash:git reset", "outside:/abs/dir", or a bare tool name ("write").
void split_scope(const std::string& scope, std::string& tool,
                 std::string& pattern) {
    std::size_t colon = scope.find(':');
    if (colon == std::string::npos) {
        tool = scope;
        pattern = "";
        return;
    }
    tool = scope.substr(0, colon);
    pattern = scope.substr(colon + 1);
}

std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream os;
    os << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%SZ");
    return os.str();
}

json rule_to_json(const PolicyRule& r) {
    return {
        {"tool", r.tool},
        {"args_pattern", r.args_pattern},
        {"level", policy_level_name(r.level)},
        {"last_choice", policy_level_name(r.last_choice)},
        {"count", r.count},
        {"created", r.created},
        {"last_used", r.last_used}
    };
}

PolicyRule json_to_rule(const json& j) {
    PolicyRule r;
    r.tool = j.value("tool", "");
    r.args_pattern = j.value("args_pattern", "");
    r.level = policy_level_from_name(j.value("level", "ask"));
    r.last_choice = policy_level_from_name(j.value("last_choice", "allow_once"));
    r.count = j.value("count", 0);
    r.created = j.value("created", "");
    r.last_used = j.value("last_used", "");
    return r;
}

} // namespace

void PolicyStore::init(const std::string& path) {
    std::ifstream f(path);
    if (f.is_open()) {
        f.close();
        load(path);
    } else {
        // Seed with default harmful patterns
        rules_ = default_harmful_patterns();
        save(path);
    }
}

void PolicyStore::load(const std::string& path) {
    rules_.clear();
    std::ifstream f(path);
    if (!f.is_open()) return;
    try {
        json j;
        f >> j;
        for (const auto& item : j) {
            rules_.push_back(json_to_rule(item));
        }
    } catch (const std::exception&) {
        (void)rules_;
    }
}

void PolicyStore::save(const std::string& path) const {
    json arr = json::array();
    for (const auto& r : rules_) {
        if (r.level != PolicyLevel::Ask)
            arr.push_back(rule_to_json(r));
    }
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp);
        if (!f) return;
        f << arr.dump(2);
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
}

const PolicyRule* PolicyStore::find(const std::string& scope_id) const {
    std::string tool, pattern;
    split_scope(scope_id, tool, pattern);
    // An exact pattern match first: "bash:rm" hits the "rm" rule even when a
    // legacy whole-tool "bash" rule also exists.
    for (const auto& r : rules_) {
        if (r.args_pattern == pattern && r.tool == tool) return &r;
    }
    // Legacy whole-tool rules (empty pattern) match a bare-tool scope id.
    if (pattern.empty()) {
        for (const auto& r : rules_) {
            if (r.args_pattern.empty() && r.tool == tool) return &r;
        }
    }
    return nullptr;
}

PolicyRule* PolicyStore::mutable_find(const std::string& scope_id) {
    std::string tool, pattern;
    split_scope(scope_id, tool, pattern);
    for (auto& r : rules_) {
        if (r.args_pattern == pattern && r.tool == tool) return &r;
    }
    if (pattern.empty()) {
        for (auto& r : rules_) {
            if (r.args_pattern.empty() && r.tool == tool) return &r;
        }
    }
    return nullptr;
}

void PolicyStore::set_rule(const std::string& scope_id, PolicyLevel level) {
    auto* existing = mutable_find(scope_id);
    if (existing) {
        existing->level = level;
        existing->last_used = timestamp();
    } else {
        PolicyRule r;
        split_scope(scope_id, r.tool, r.args_pattern);
        r.level = level;
        r.last_choice = PolicyLevel::AllowOnce;
        r.created = timestamp();
        r.last_used = timestamp();
        rules_.push_back(std::move(r));
    }
}

void PolicyStore::revoke(const std::string& scope_id) {
    std::string tool, pattern;
    split_scope(scope_id, tool, pattern);
    auto it = std::remove_if(rules_.begin(), rules_.end(),
        [&](const PolicyRule& r) {
            return r.tool == tool &&
                   (pattern.empty() || r.args_pattern == pattern);
        });
    rules_.erase(it, rules_.end());
    session_grants_.erase(scope_id);
}

void PolicyStore::record_choice(const std::string& scope_id, PolicyLevel choice) {
    auto* existing = mutable_find(scope_id);
    if (existing) {
        existing->last_choice = choice;
        existing->count++;
        existing->last_used = timestamp();
    } else {
        PolicyRule r;
        split_scope(scope_id, r.tool, r.args_pattern);
        r.level = PolicyLevel::Ask;
        r.last_choice = choice;
        r.count = 1;
        r.created = timestamp();
        r.last_used = timestamp();
        rules_.push_back(std::move(r));
    }
}

bool PolicyStore::is_granted_session(const std::string& scope_id) const {
    return session_grants_.count(scope_id) > 0;
}

void PolicyStore::grant_session(const std::string& scope_id) {
    session_grants_.insert(scope_id);
}

void PolicyStore::clear_session() {
    session_grants_.clear();
}

std::vector<PolicyRule> PolicyStore::default_harmful_patterns() {
    std::vector<PolicyRule> list;
    auto add = [&](const std::string& tool, const std::string& pat) {
        PolicyRule r;
        r.tool = tool;
        r.args_pattern = pat;
        r.level = PolicyLevel::Ask;
        r.last_choice = PolicyLevel::AllowOnce;
        list.push_back(std::move(r));
    };
    for (const std::string& pat : destructive_command_patterns())
        add("bash", pat);
    add("process_start", "");
    add("process_stop", "");
    return list;
}

} // namespace agent
