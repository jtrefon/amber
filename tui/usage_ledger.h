#ifndef AMBER_TUI_USAGE_LEDGER_H
#define AMBER_TUI_USAGE_LEDGER_H

#include "agent/extensions.h"

namespace tui {

// A chat window's spend, split by source so the tree total is attributable.
// `main` is this conversation's own requests; `subagents`, `compression` and
// `watch` are work it caused; `inherited` is the baseline a fork started from.
struct UsageLedger {
    agent::UsageTotals main;
    agent::UsageTotals subagents;
    agent::UsageTotals compression;
    agent::UsageTotals watch;
    agent::UsageTotals inherited;

    agent::UsageTotals tree() const {
        agent::UsageTotals out;
        const agent::UsageTotals* parts[] = {&main, &subagents, &compression, &watch, &inherited};
        for (const auto* part : parts) {
            out.prompt_tokens += part->prompt_tokens;
            out.completion_tokens += part->completion_tokens;
            out.measured = out.measured || part->measured;
        }
        return out;
    }

    // `/usage reset`: an explicit user action zeroes the whole tree, inherited
    // baseline included.
    void reset() { *this = UsageLedger{}; }
};

} // namespace tui

#endif // AMBER_TUI_USAGE_LEDGER_H
