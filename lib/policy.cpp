
#include "agent/policy.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace agent {

static std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::ostringstream os;
    os << std::put_time(std::gmtime(&t), "%Y-%m-%dT%H:%M:%SZ");
    return os.str();
}

static json rule_to_json(const PolicyRule& r) {
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

static PolicyRule json_to_rule(const json& j) {
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

const PolicyRule* PolicyStore::find(const std::string& tool) const {
    for (const auto& r : rules_) {
        if (r.tool == tool && r.args_pattern.empty()) {
            return &r;
        }
    }
    return nullptr;
}

PolicyRule* PolicyStore::mutable_find(const std::string& tool) {
    for (auto& r : rules_) {
        if (r.tool == tool && r.args_pattern.empty()) return &r;
    }
    return nullptr;
}

void PolicyStore::set_rule(const std::string& tool, PolicyLevel level) {
    auto* existing = mutable_find(tool);
    if (existing) {
        existing->level = level;
        existing->last_used = timestamp();
    } else {
        PolicyRule r;
        r.tool = tool;
        r.args_pattern = "";
        r.level = level;
        r.last_choice = PolicyLevel::AllowOnce;
        r.created = timestamp();
        r.last_used = timestamp();
        rules_.push_back(std::move(r));
    }
}

void PolicyStore::revoke(const std::string& tool) {
    auto it = std::remove_if(rules_.begin(), rules_.end(),
        [&](const PolicyRule& r) { return r.tool == tool; });
    rules_.erase(it, rules_.end());
    last_choices_.erase(tool);
}

void PolicyStore::record_choice(const std::string& tool, PolicyLevel choice) {
    last_choices_[tool] = choice;
    auto* existing = mutable_find(tool);
    if (existing) {
        existing->last_choice = choice;
        existing->count++;
        existing->last_used = timestamp();
    } else {
        PolicyRule r;
        r.tool = tool;
        r.args_pattern = "";
        r.level = PolicyLevel::Ask;
        r.last_choice = choice;
        r.count = 1;
        r.created = timestamp();
        r.last_used = timestamp();
        rules_.push_back(std::move(r));
    }
}

bool PolicyStore::is_granted_session(const std::string& tool) const {
    return session_grants_.count(tool) > 0;
}

void PolicyStore::grant_session(const std::string& tool) {
    session_grants_.insert(tool);
}

void PolicyStore::clear_session() {
    session_grants_.clear();
    last_choices_.clear();
}

PolicyLevel PolicyStore::last_choice(const std::string& tool) const {
    auto it = last_choices_.find(tool);
    if (it != last_choices_.end()) return it->second;
    const auto* rule = find(tool);
    if (rule && rule->last_choice != PolicyLevel::Ask)
        return rule->last_choice;
    return PolicyLevel::AllowOnce;
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
    add("bash", "rm");
    add("bash", "rmdir");
    add("bash", "mv");
    add("bash", "cp");
    add("bash", "dd");
    add("bash", "mkfs");
    add("bash", "fdisk");
    add("bash", "chmod");
    add("bash", "chown");
    add("bash", "sudo");
    add("bash", "apt");
    add("bash", "apt-get");
    add("bash", "kill");
    add("bash", "pkill");
    add("bash", "git reset");
    add("bash", "git push --force");
    add("bash", "git clean");
    add("bash", "git revert");
    add("bash", "pip install");
    add("bash", "npm install");
    add("bash", "npm publish");
    add("bash", "docker");
    add("bash", "systemctl");
    add("bash", "sed -i");
    add("bash", ">");
    add("bash", ">>");
    add("bash", "|");
    add("process_start", "");
    add("process_stop", "");
    return list;
}

} // namespace agent
