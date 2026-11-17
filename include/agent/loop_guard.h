#ifndef AGENT_LOOP_GUARD_H
#define AGENT_LOOP_GUARD_H

#include <functional>
#include <string>

#include "agent/llm.h" // json

namespace agent {

struct Config;
class Context;
class ContextEventSource;
class ConversationLog;
class Turn;
struct AgentHooks;
struct Message;

// The turn loop's loop policy: repeated narration, a repeated dispatch
// fingerprint, a tool that keeps failing, and the budget notices.
//
// The guard decides and reports; it never runs a tool or a request. The agent
// keeps the dispatcher, the conversation and the host wiring, and hands the
// guard the turn state to judge.
class LoopGuard {
public:
    // How a notice reaches the conversation: the agent pushes it, publishes the
    // context change and announces the new message, exactly as it does for
    // every harness-authored notice.
    using NoticeSink = std::function<void(const std::string&)>;

    LoopGuard(const Config& cfg, const AgentHooks& hooks, ConversationLog& log, Context& context,
              ContextEventSource& events, NoticeSink notice);

    // The tooling-phase announcements, once per dispatch: narration, the
    // Tooling state, and the call count for the debug log.
    void announce_dispatch(const json& calls, const std::string& content) const;

    // Budget and repeated-call notices, at most once each per turn. They
    // announce a bound; they do not enforce it.
    void note_state(Turn& turn, int iteration);

    // True when the same tool-call fingerprint has repeated the guard's limit:
    // the turn carries the stop reason and must not dispatch again.
    bool stop_dispatch_loop(const json& calls, Turn& turn);

    // Repeated narration. The second occurrence earns one corrective note; a
    // repeat past recovery stops the turn and returns true.
    bool repeated_narration(const std::string& content, Turn& turn);

    // Fold one dispatch's outcome into the fail-streak ladder: steer the model
    // off a failing tool once, then hard-stop.
    void fold_fail_streak(const json& calls, bool dispatch_ok, Turn& turn);

private:
    const Config& cfg_;
    const AgentHooks& hooks_;
    ConversationLog& log_;
    Context& context_;
    ContextEventSource& events_;
    NoticeSink notice_;
};

} // namespace agent

#endif // AGENT_LOOP_GUARD_H
