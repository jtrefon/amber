#ifndef AMBER_TUI_USAGE_REPORT_H
#define AMBER_TUI_USAGE_REPORT_H

#include <string>
#include <utility>
#include <vector>

#include "agent/compact.h"
#include "agent/subagent.h"
#include "usage_ledger.h"

namespace tui {

namespace detail {

inline std::string count_label(const agent::UsageTotals& usage) {
    return "up " + agent::compact::count(usage.prompt_tokens) + " down " +
           agent::compact::count(usage.completion_tokens);
}

} // namespace detail

// One "label: up X down Y" line; unmeasured sources say so.
inline std::string usage_line(const char* label, const agent::UsageTotals& usage) {
    std::string line = std::string(label) + ": " + detail::count_label(usage);
    if (!usage.measured)
        line += " (not measured)";
    return line;
}

// The /usage show body: tree, main, measured sub-sources, per-task lines for
// sub-agents that reported usage, and every chat window's total when more than
// one window exists. Pure, so the formatting is unit-tested without a terminal.
inline std::vector<std::string>
usage_report_lines(const UsageLedger& ledger, const std::vector<agent::SubAgentTaskInfo>& tasks,
                   const std::vector<std::pair<std::string, agent::UsageTotals>>& windows) {
    std::vector<std::string> lines;
    lines.push_back(usage_line("tree", ledger.tree()));
    lines.push_back(usage_line("main", ledger.main));
    if (ledger.subagents.measured)
        lines.push_back(usage_line("sub-agents", ledger.subagents));
    if (ledger.compression.measured)
        lines.push_back(usage_line("compression", ledger.compression));
    if (ledger.watch.measured)
        lines.push_back(usage_line("watches", ledger.watch));
    if (ledger.inherited.measured)
        lines.push_back(usage_line("inherited", ledger.inherited));
    for (const auto& task : tasks) {
        if (!task.usage_measured)
            continue;
        agent::UsageTotals usage;
        usage.prompt_tokens = task.prompt_tokens;
        usage.completion_tokens = task.completion_tokens;
        usage.measured = true;
        lines.push_back("task " + task.id + ": " + detail::count_label(usage));
    }
    if (windows.size() > 1) {
        for (const auto& [title, usage] : windows)
            lines.push_back("window '" + title + "': " + detail::count_label(usage));
    }
    return lines;
}

} // namespace tui

#endif // AMBER_TUI_USAGE_REPORT_H
