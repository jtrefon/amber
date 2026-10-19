#ifndef AMBER_TUI_SESSION_USAGE_H
#define AMBER_TUI_SESSION_USAGE_H

#include "agent/extensions.h"
#include "agent/session.h"

// Session spend survives a reload. Kept apart from the session controller so
// the round trip is testable without a terminal: the two functions only touch
// the meta object and the totals.
namespace tui {

inline void save_usage(agent::json& meta, const agent::UsageTotals& usage) {
    meta["session_prompt_tokens"] = usage.prompt_tokens;
    meta["session_completion_tokens"] = usage.completion_tokens;
    meta["session_usage_measured"] = usage.measured;
}

inline void load_usage(const agent::json& meta, agent::UsageTotals& usage) {
    const auto number = [&meta](const char* key) -> long {
        return meta.contains(key) && meta[key].is_number() ? meta[key].get<long>() : 0;
    };
    usage.prompt_tokens = number("session_prompt_tokens");
    usage.completion_tokens = number("session_completion_tokens");
    usage.measured =
        meta.contains("session_usage_measured") && meta["session_usage_measured"].is_boolean()
            ? meta["session_usage_measured"].get<bool>()
            : false;
}

} // namespace tui

#endif // AMBER_TUI_SESSION_USAGE_H
