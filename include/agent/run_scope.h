#ifndef AGENT_RUN_SCOPE_H
#define AGENT_RUN_SCOPE_H

#include <vector>

#include "agent/activation_sink.h"
#include "agent/process.h"
#include "agent/skill_catalog.h"

namespace agent {

// The resources a tool resolves from the RUNNING agent — not from whatever
// object was bound at tool registration. One frame per run.
struct RunScope {
    const CancellationToken* cancel_token = nullptr;
    SkillCatalog* skills = nullptr;
    ActivationSink* activated = nullptr;
};

// The active scope for a thread, flattened into a value: the leaf frame plus
// the resolved ancestor cancel tokens (leaf-first). A value type, so it can be
// captured on one thread and installed on another — the property the tool
// dispatch path needs, since dispatch runs tools on std::async workers rather
// than the agent thread. Flattening at capture time removes the shared,
// mutable parent link: installing a chain never touches another thread's
// objects, so it is safe to share a captured chain with concurrent workers.
struct RunScopeChain {
    RunScope leaf;
    std::vector<const CancellationToken*> cancel_chain; // leaf-first

    bool empty() const noexcept {
        return cancel_chain.empty() && leaf.skills == nullptr && leaf.activated == nullptr;
    }
};

inline thread_local const RunScopeChain* t_run_scope = nullptr;

inline const RunScopeChain* current_run_scope() noexcept {
    return t_run_scope;
}

// Snapshot the calling thread's chain. Empty when no scope is installed.
inline RunScopeChain capture_run_scope() {
    return t_run_scope ? *t_run_scope : RunScopeChain{};
}

// RAII install. Two forms:
//  - from a leaf frame: builds a chain from the calling thread's current scope
//    and installs it (the agent worker installing its own run);
//  - from a captured chain: installs a value another thread produced (a
//    dispatch worker adopting the calling agent's scope). The chain must
//    outlive the guard — bind it to a named local or a lambda capture.
class ScopedRunScope {
public:
    explicit ScopedRunScope(const RunScope& leaf) : prev_(t_run_scope), installed_(&owned_) {
        owned_.leaf = leaf;
        if (prev_)
            owned_.cancel_chain = prev_->cancel_chain;
        if (leaf.cancel_token)
            owned_.cancel_chain.insert(owned_.cancel_chain.begin(), leaf.cancel_token);
        t_run_scope = installed_;
    }

    explicit ScopedRunScope(const RunScopeChain& chain) : prev_(t_run_scope), installed_(&chain) {
        t_run_scope = chain.empty() ? prev_ : installed_;
    }

    ScopedRunScope(RunScopeChain&&) = delete; // no temporaries: the address would dangle
    ~ScopedRunScope() { t_run_scope = prev_; }

    ScopedRunScope(const ScopedRunScope&) = delete;
    ScopedRunScope& operator=(const ScopedRunScope&) = delete;

private:
    const RunScopeChain* prev_;
    RunScopeChain owned_;
    const RunScopeChain* installed_;
};

// The catalog a skill tool should serve: the running agent's catalog when a
// scope is installed, otherwise the catalog bound at registration.
inline SkillCatalog& effective_catalog(SkillCatalog& fallback) noexcept {
    const RunScopeChain* s = t_run_scope;
    return (s && s->leaf.skills) ? *s->leaf.skills : fallback;
}

// Record a session activation on the running agent's sink; a no-op outside a
// run (the catalog still served the body — only the prompt-copy re-injection
// bookkeeping is skipped).
inline void record_activation(const std::string& name, const std::string& body) {
    const RunScopeChain* s = t_run_scope;
    if (s && s->leaf.activated)
        s->leaf.activated->record(name, body);
}

// Cancellation check that consults the scope chain: a tool or agent loop
// aborts when ITS run's token — or any ancestor run's (a window whose
// in-flight tool spawned this sub-agent run) — was requested. With no scope
// installed the bound fallback token decides (CLI/headless parity).
inline bool run_cancelled(const CancellationToken& fallback) noexcept {
    if (const RunScopeChain* s = t_run_scope) {
        for (const CancellationToken* t : s->cancel_chain)
            if (t && t->is_requested())
                return true;
    }
    return fallback.is_requested();
}

} // namespace agent

#endif // AGENT_RUN_SCOPE_H
