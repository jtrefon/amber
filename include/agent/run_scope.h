#ifndef AGENT_RUN_SCOPE_H
#define AGENT_RUN_SCOPE_H

#include <string>
#include <vector>

#include "agent/process.h"
#include "agent/skill_catalog.h"

namespace agent {

// Per-run ambient context, installed by Agent::run for the duration of a
// turn (thread-local). Tools resolve the CALLING agent's resources through
// it — the cancel token, skill catalog, and skill-activation sink follow
// whichever agent runs on this thread, not whichever object was bound at
// tool registration. This is what lets one shared ToolRegistry serve many
// concurrent, independent agents (TUI windows, sub-agents) without
// cross-talk.
//
// `activated` points at the running agent's session-activated skill list
// (catalog data is shared per project; activation is per-session — a skill
// read in one window must not inject into a sibling's prompts).
//
// `parent` links nested scopes: a sub-agent run inside a tool installs its
// own scope but remains cancellable through its ancestors' tokens
// (run_cancelled walks the chain).
//
// nullptr outside a run (CLI before run, tests, headless): every consumer
// falls back to the object bound at registration, so behavior without a
// scope is exactly the pre-scope behavior.
struct RunScope {
    const CancellationToken* cancel_token = nullptr;
    SkillCatalog* skills = nullptr;
    std::vector<ActivatedSkill>* activated = nullptr;
    const RunScope* parent = nullptr;
};

inline thread_local RunScope* t_run_scope = nullptr;

inline RunScope* current_run_scope() noexcept {
    return t_run_scope;
}

// RAII install/restore. Nested runs (an agent run inside a tool that spawns
// a sub-agent) restore the outer scope when the inner one ends.
class ScopedRunScope {
public:
    explicit ScopedRunScope(RunScope* s) noexcept : prev_(t_run_scope) {
        if (s)
            s->parent = prev_;
        t_run_scope = s;
    }
    ~ScopedRunScope() { t_run_scope = prev_; }

    ScopedRunScope(const ScopedRunScope&) = delete;
    ScopedRunScope& operator=(const ScopedRunScope&) = delete;

private:
    RunScope* prev_;
};

// The catalog a skill tool should serve: the running agent's catalog when a
// scope is installed, otherwise the catalog bound at registration.
inline SkillCatalog& effective_catalog(SkillCatalog& fallback) noexcept {
    RunScope* s = current_run_scope();
    return (s && s->skills) ? *s->skills : fallback;
}

// Record a session activation on the running agent's sink; a no-op outside
// a run (the catalog still served the body — only the prompt-copy
// re-injection bookkeeping is skipped).
inline void record_activation(const std::string& name, const std::string& body) {
    RunScope* s = current_run_scope();
    if (!s || !s->activated)
        return;
    for (const auto& a : *s->activated)
        if (a.name == name)
            return; // already active this session
    s->activated->push_back({name, body});
}

// Cancellation check that walks the scope chain: a cancellable tool or an
// agent loop aborts when ITS run's token — or any ancestor run's (a window
// whose in-flight tool spawned this sub-agent run) — was requested. With no
// scope installed the bound fallback token decides (CLI/headless parity).
inline bool run_cancelled(const CancellationToken& fallback) noexcept {
    const RunScope* s = current_run_scope();
    while (s) {
        if (s->cancel_token && s->cancel_token->is_requested())
            return true;
        s = s->parent;
    }
    return fallback.is_requested();
}

} // namespace agent

#endif // AGENT_RUN_SCOPE_H
