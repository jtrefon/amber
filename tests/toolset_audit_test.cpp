// The toolset audit (tools-domain spec §5.2): deficiency and ambiguity
// warnings, never a gate. Hermetic - a registry and some stub tools.

#include "agent.h"
#include "agent/registry.h"
#include "agent/toolset_audit.h"
#include "test_util.h"

#include <cstdio>
#include <ostream>
#include <memory>
#include <string>
#include <vector>

using namespace agent;

// Printable for the assertion macro, so a failure names the role rather than
// refusing to compile.
namespace agent {
std::ostream& operator<<(std::ostream& os, ToolRole role) {
    return os << to_string(role);
}
} // namespace agent

namespace {

class StubTool : public Tool {
public:
    StubTool(std::string name, std::string description)
        : name_(std::move(name)), description_(std::move(description)) {}

    std::string name() const noexcept override { return name_; }
    std::string description() const noexcept override { return description_; }
    json parameters_schema() const override { return json::object(); }
    ToolResult execute(const json&) const override { return {true, "", "", json{}}; }

private:
    std::string name_;
    std::string description_;
};

void add(ToolRegistry& reg, const std::string& name, const std::string& description,
         ToolRole role) {
    reg.register_tool(std::make_unique<StubTool>(name, description), "test",
                      ToolMeta{"running", role});
}

std::size_t count_of(const std::vector<AuditFinding>& findings,
                     AuditFinding::Kind kind) {
    std::size_t n = 0;
    for (const auto& f : findings)
        if (f.kind == kind) ++n;
    return n;
}

} // namespace

TEST(audit_role_names_round_trip) {
    for (ToolRole role : {ToolRole::Other, ToolRole::Read, ToolRole::Write,
                          ToolRole::Search, ToolRole::Execute, ToolRole::Plan,
                          ToolRole::Delegate}) {
        ASSERT_EQ(parse_tool_role(to_string(role)), role);
    }
    ASSERT_EQ(parse_tool_role("nonsense"), ToolRole::Other);
    ASSERT_EQ(parse_tool_role(""), ToolRole::Other);
}

TEST(audit_reports_a_missing_required_role) {
    ToolRegistry reg;
    add(reg, "bash", "Run a shell command.", ToolRole::Execute);

    auto findings = audit_toolset(reg);
    const auto deficiencies = count_of(findings, AuditFinding::Kind::Deficiency);
    ASSERT_EQ(deficiencies, 1u);
    // The message says which ability is gone and how to get it back.
    ASSERT(findings[0].message.find("read") != std::string::npos);
    ASSERT(findings[0].message.find("/get plugin") != std::string::npos);
}

TEST(audit_is_silent_when_a_read_tool_is_present) {
    ToolRegistry reg;
    add(reg, "read", "Read a file from the workspace.", ToolRole::Read);
    ASSERT_TRUE(audit_toolset(reg).empty());
}

TEST(audit_reports_two_tools_that_describe_the_same_job) {
    ToolRegistry reg;
    add(reg, "read", "Read a file from the workspace.", ToolRole::Read);
    add(reg, "search_grep", "Search the workspace for a pattern.", ToolRole::Search);
    add(reg, "search_index", "Search the workspace for a pattern.", ToolRole::Search);

    auto findings = audit_toolset(reg);
    ASSERT_EQ(count_of(findings, AuditFinding::Kind::Ambiguity), 1u);
    ASSERT_EQ(count_of(findings, AuditFinding::Kind::Deficiency), 0u);
    ASSERT(findings[0].message.find("search_grep") != std::string::npos);
    ASSERT(findings[0].message.find("search_index") != std::string::npos);
}

// The point of the rule: two tools in one category are fine when the model can
// tell them apart, which is exactly the grep-vs-semantic comparison this
// domain exists to enable.
TEST(audit_allows_two_tools_that_distinguish_themselves) {
    ToolRegistry reg;
    add(reg, "read", "Read a file from the workspace.", ToolRole::Read);
    add(reg, "search", "Fast regex scan across files using grep.", ToolRole::Search);
    add(reg, "search_semantic",
        "Dependency-free lexical index ranked by term frequency.", ToolRole::Search);

    ASSERT_TRUE(audit_toolset(reg).empty());
}

TEST(audit_ignores_shared_role_where_the_category_is_open) {
    // Two `Other` tools share no declared job, so there is nothing to be
    // ambiguous about - a category nobody else shares is a complete answer.
    ToolRegistry reg;
    add(reg, "read", "Read a file from the workspace.", ToolRole::Read);
    add(reg, "weather", "Report the forecast for a city.", ToolRole::Other);
    add(reg, "clock", "Report the current time.", ToolRole::Other);

    ASSERT_TRUE(audit_toolset(reg).empty());
}

// The acceptance test for the whole rule: the set amber actually ships must not
// trip its own audit. A warning system that fires on the default configuration
// is noise, and noise is what teaches people to ignore warnings.
TEST(audit_the_shipped_tool_set_is_clean) {
    ToolRegistry reg;
    JobService jobs;
    TodoStore todos;
    SubAgentExecutor subagents;
    register_default_tools(reg, jobs, todos, CancellationToken{}, subagents);

    auto findings = audit_toolset(reg);
    std::string joined;
    for (const auto& f : findings) joined += to_string(f.kind) + ": " + f.message + "\n";
    ASSERT_EQ(findings.size(), 0u);

    // Every shipped tool also declares a role, so none of them is invisible to
    // the audit (an undeclared tool would silently weaken it).
    std::vector<std::string> undeclared;
    for (const auto& tool : reg.snapshot_tools()) {
        if (reg.meta_for(tool->name()).role == ToolRole::Other)
            undeclared.push_back(tool->name());
    }
    for (const auto& name : undeclared)
        std::printf("tool '%s' declares no role\n", name.c_str());
    ASSERT_EQ(undeclared.size(), 0u);
}

TEST(audit_exposes_a_real_disabling_consequence) {
    // Disabling the read tool is the deficiency the spec named: the harness
    // keeps working, and can no longer bring back content.
    ToolRegistry reg;
    JobService jobs;
    TodoStore todos;
    SubAgentExecutor subagents;
    register_default_tools(reg, jobs, todos, CancellationToken{}, subagents);
    ASSERT_TRUE(audit_toolset(reg).empty());

    ASSERT_TRUE(reg.remove_tool("read"));
    auto findings = audit_toolset(reg);
    ASSERT_EQ(count_of(findings, AuditFinding::Kind::Deficiency), 1u);
    ASSERT(findings[0].message.find("read") != std::string::npos);
}
