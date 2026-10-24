#ifndef AMBER_TUI_SESSION_USAGE_H
#define AMBER_TUI_SESSION_USAGE_H

#include "agent/extensions.h"
#include "agent/session.h"
#include "usage_ledger.h"

// Session spend survives a reload. Kept apart from the session controller so
// the round trip is testable without a terminal: the functions only touch the
// meta object and the ledger. The legacy `session_prompt_tokens` /
// `session_completion_tokens` keys stay the tree total, so a session written by
// an older build restores as `main` and a newer session still reads in an
// older build.
namespace tui {

namespace detail {

inline void save_source(agent::json& meta, const char* prefix, const agent::UsageTotals& usage) {
    const std::string p(prefix);
    meta[p + "_prompt_tokens"] = usage.prompt_tokens;
    meta[p + "_completion_tokens"] = usage.completion_tokens;
    meta[p + "_measured"] = usage.measured;
}

inline agent::UsageTotals load_source(const agent::json& meta, const char* prefix) {
    const auto number = [&meta](const std::string& key) -> long {
        return meta.contains(key) && meta[key].is_number() ? meta[key].get<long>() : 0;
    };
    const auto flag = [&meta](const std::string& key) -> bool {
        return meta.contains(key) && meta[key].is_boolean() ? meta[key].get<bool>() : false;
    };
    const std::string p(prefix);
    agent::UsageTotals usage;
    usage.prompt_tokens = number(p + "_prompt_tokens");
    usage.completion_tokens = number(p + "_completion_tokens");
    usage.measured = flag(p + "_measured");
    return usage;
}

} // namespace detail

inline void save_usage(agent::json& meta, const UsageLedger& ledger) {
    detail::save_source(meta, "session_usage_main", ledger.main);
    detail::save_source(meta, "session_usage_subagents", ledger.subagents);
    detail::save_source(meta, "session_usage_compression", ledger.compression);
    detail::save_source(meta, "session_usage_watch", ledger.watch);
    detail::save_source(meta, "session_usage_inherited", ledger.inherited);
    const agent::UsageTotals tree = ledger.tree();
    meta["session_prompt_tokens"] = tree.prompt_tokens;
    meta["session_completion_tokens"] = tree.completion_tokens;
    meta["session_usage_measured"] = tree.measured;
}

inline void load_usage(const agent::json& meta, UsageLedger& ledger) {
    ledger = UsageLedger{};
    ledger.main = detail::load_source(meta, "session_usage_main");
    ledger.subagents = detail::load_source(meta, "session_usage_subagents");
    ledger.compression = detail::load_source(meta, "session_usage_compression");
    ledger.watch = detail::load_source(meta, "session_usage_watch");
    ledger.inherited = detail::load_source(meta, "session_usage_inherited");
    if (!ledger.main.measured) {
        // Older builds stored only the tree total under these keys.
        const auto number = [&meta](const char* key) -> long {
            return meta.contains(key) && meta[key].is_number() ? meta[key].get<long>() : 0;
        };
        const bool measured =
            meta.contains("session_usage_measured") && meta["session_usage_measured"].is_boolean()
                ? meta["session_usage_measured"].get<bool>()
                : false;
        if (measured) {
            ledger.main.prompt_tokens = number("session_prompt_tokens");
            ledger.main.completion_tokens = number("session_completion_tokens");
            ledger.main.measured = true;
        }
    }
}

} // namespace tui

#endif // AMBER_TUI_SESSION_USAGE_H
