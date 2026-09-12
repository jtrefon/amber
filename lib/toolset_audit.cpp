#include "agent/toolset_audit.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>

namespace agent {

namespace {

// Words that carry no distinguishing information in a tool description. Kept
// small on purpose: an aggressive stop list would make two genuinely different
// descriptions look identical and manufacture false ambiguity warnings.
bool is_significant_word(const std::string& w) {
    static const std::set<std::string> kNoise = {
        "a", "an", "and", "as", "at", "be", "by", "for", "from", "in", "into",
        "is", "it", "of", "on", "or", "that", "the", "this", "to", "use",
        "used", "using", "with", "you", "your"};
    return w.size() > 2 && kNoise.find(w) == kNoise.end();
}

std::set<std::string> significant_words(const std::string& text) {
    std::set<std::string> words;
    std::string current;
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            current += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (!current.empty()) {
            if (is_significant_word(current)) words.insert(current);
            current.clear();
        }
    }
    if (!current.empty() && is_significant_word(current)) words.insert(current);
    return words;
}

// Jaccard similarity of the two descriptions' significant words: how much of
// what they say is the same. Threshold set where "describes the same job in
// the same terms" begins, not where paraphrase does.
constexpr double kSameDescription = 0.6;

double similarity(const std::set<std::string>& a, const std::set<std::string>& b) {
    if (a.empty() || b.empty()) return 0.0;
    std::vector<std::string> shared;
    std::set_intersection(a.begin(), a.end(), b.begin(), b.end(),
                          std::back_inserter(shared));
    const std::size_t total = a.size() + b.size() - shared.size();
    if (total == 0) return 0.0;
    return static_cast<double>(shared.size()) / static_cast<double>(total);
}

// The tools a role is carried by, in registry order (deterministic output).
struct RoleUsage {
    ToolRole role = ToolRole::Other;
    std::vector<std::shared_ptr<Tool>> tools;
    std::vector<ToolMeta> meta;
};

std::map<ToolRole, RoleUsage> group_by_role(const ToolRegistry& registry) {
    std::map<ToolRole, RoleUsage> by_role;
    for (const auto& tool : registry.snapshot_tools()) {
        if (!tool) continue;
        const ToolMeta meta = registry.meta_for(tool->name());
        RoleUsage& usage = by_role[meta.role];
        usage.role = meta.role;
        usage.tools.push_back(tool);
        usage.meta.push_back(meta);
    }
    return by_role;
}

void append_deficiencies(const std::map<ToolRole, RoleUsage>& by_role,
                         std::vector<AuditFinding>& out) {
    for (ToolRole required : required_tool_roles()) {
        if (by_role.find(required) != by_role.end()) continue;
        AuditFinding f;
        f.kind = AuditFinding::Kind::Deficiency;
        f.message = "no enabled tool can " + to_string(required) +
                    " - the model has lost that ability; enable a plugin that "
                    "provides it (/get plugin)";
        out.push_back(std::move(f));
    }
}

void append_ambiguities(const std::map<ToolRole, RoleUsage>& by_role,
                        std::vector<AuditFinding>& out) {
    for (const auto& [role, usage] : by_role) {
        if (role == ToolRole::Other || usage.tools.size() < 2) continue;
        for (std::size_t i = 0; i < usage.tools.size(); ++i) {
            for (std::size_t j = i + 1; j < usage.tools.size(); ++j) {
                const auto left = significant_words(usage.tools[i]->description());
                const auto right = significant_words(usage.tools[j]->description());
                if (similarity(left, right) < kSameDescription) continue;
                AuditFinding f;
                f.kind = AuditFinding::Kind::Ambiguity;
                f.message = "tools '" + usage.tools[i]->name() + "' and '" +
                            usage.tools[j]->name() + "' both " + to_string(role) +
                            " and describe themselves the same way - the model "
                            "cannot tell which to use";
                out.push_back(std::move(f));
            }
        }
    }
}

} // namespace

std::string to_string(AuditFinding::Kind kind) {
    return kind == AuditFinding::Kind::Deficiency ? "deficiency" : "ambiguity";
}

std::vector<ToolRole> required_tool_roles() {
    return {ToolRole::Read};
}

std::vector<AuditFinding> audit_toolset(const ToolRegistry& registry) {
    const auto by_role = group_by_role(registry);
    std::vector<AuditFinding> out;
    append_deficiencies(by_role, out);
    append_ambiguities(by_role, out);
    return out;
}

} // namespace agent
