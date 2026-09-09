
#ifndef AGENT_SESSION_BRIEF_H
#define AGENT_SESSION_BRIEF_H

// Host-owned session brief: a structured narrative (intent / direction /
// done / next / avoid) that survives context compression. The brief lives
// here, not in the conversation context, so compression cannot erode it.
// Re-injected into the per-request prompt copy on every chat_once — the
// same pattern as memory injection. See docs/plan/session-brief.md.

#include <string>
#include <vector>

namespace agent {

constexpr size_t kBriefMaxDone = 10;
constexpr size_t kBriefMaxAvoid = 20;
constexpr size_t kBriefMaxRenderBytes = 2048;

struct SessionBrief {
    std::string intent;
    std::string direction;
    std::vector<std::string> done;
    std::string earlier;
    std::string next;
    std::vector<std::string> avoid;
};

class SessionBriefStore {
public:
    SessionBriefStore() = default;

    bool load(const std::string& path);
    bool save(const std::string& path) const;

    // Merge a freshly-extracted brief into the store.
    // Intent / Direction / Next: replace. Done: append, cap at kBriefMaxDone,
    // fold older into `earlier`. Avoid: append-only, cap at kBriefMaxAvoid.
    void merge(const SessionBrief& fresh);

    // Render the brief as a [session-brief]...[/session-brief] block,
    // capped at kBriefMaxRenderBytes.
    std::string render() const;

    bool empty() const noexcept;
    void clear() noexcept;

private:
    SessionBrief brief_;
};

} // namespace agent

#endif // AGENT_SESSION_BRIEF_H
