#ifndef AGENT_TURN_H
#define AGENT_TURN_H

#include <chrono>
#include <string>

#include "agent/llm.h"           // json
#include "agent/tool_recovery.h" // FailStreak

namespace agent {

// What the loop does about a reply that repeats the previous one.
enum class TextRepeat {
    None,         // first occurrence, or a repeat still inside the steer window
    Steer,        // second occurrence: hand the model one corrective note
    Unrecoverable // repeated past the point where steering helps
};

// One turn's mutable state: the loop's counters, the recovery ladder's
// bookkeeping, the budgets, and the reply the turn will return.
//
// The loop and its helpers change it only through these methods, so each
// transition rule is written once and named for its intent instead of being
// spread across the site that happened to touch a field. Everything the loop
// reports (step budget, wall clock, repeated call, silence) is derived here,
// which is what keeps the reported state and the enforced state the same.
class Turn {
public:
    // Silence: consecutive empty replies with no tool call.
    static constexpr int kMaxSilence = 2;
    // Harness notices in a row with no model reply between them: a failing
    // endpoint has its own patience, separate from the model going quiet.
    static constexpr int kMaxNotices = 3;
    // Failures raised while folding a reply, before the turn gives up.
    static constexpr int kMaxInternalFailures = 2;
    // Identical narration: the second occurrence earns a steer, the fifth ends
    // the turn.
    static constexpr int kTextSteerAt = 2;
    static constexpr int kTextStopAt = 5;
    // An identical dispatch fingerprint this many times in a row is a loop.
    static constexpr int kDispatchLoopAt = 3;
    // A failing tool steers the model off it once, then hard-stops.
    static constexpr int kFailStreakAt = 3;
    // How far into a budget the warning goes out, and when a repeated call is
    // worth mentioning.
    static constexpr int kBudgetNotePct = 80;
    static constexpr int kRepeatNoteAt = 2;

    Turn(std::chrono::steady_clock::time_point start, int max_wall_ms);

    // The model is working: patience with silence and with harness notices
    // resets, and the turn is marked as having run tools.
    void record_tool_reply();

    // Repeated narration. `None` also covers a fresh reply (which resets the
    // count) and the repeats between the steer and the stop.
    TextRepeat observe_text(const std::string& content);

    // One canonical call (arguments normalised, timing stripped). Returns the
    // run length; this is the reporting counter, it never intervenes.
    int observe_repeated_call(const std::string& canonical_key);

    // One dispatch fingerprint. Returns true when it is the loop guard's
    // limit repeated call, which ends the turn.
    bool record_dispatch_fingerprint(const std::string& fingerprint);

    // Fold a tool batch into the fail-streak ladder; returns the worst streak.
    int record_tool_result(const json& tool_calls, bool all_ok);

    // Claim the one recovery steer. False means the ladder is spent and the
    // turn must stop.
    bool claim_recovery_steer();

    // A report from the environment: the model has the next word. True when
    // the notices are exhausted.
    bool record_notice();

    // An empty reply with no tool call. True when the turn ran out of patience.
    bool record_silent_reply();

    // A non-chat failure raised while folding a reply. True when the turn must
    // stop instead of recovering again.
    bool record_internal_failure();

    // Wall-clock budget, checked before each request.
    bool wall_clock_exceeded(std::chrono::steady_clock::time_point now) const;
    long elapsed_ms(std::chrono::steady_clock::time_point now) const;

    // Each returns true exactly once per turn, when the warning is due. They
    // announce, they do not intervene.
    bool note_iteration_budget(int iteration, int max_iterations);
    bool note_wall_budget(int max_wall_ms, std::chrono::steady_clock::time_point now);
    int note_repeated_call();

    // An internal failure that was survived: the stop reason names the last
    // one.
    void record_failure(const std::string& failure);

    // The model closed the turn with its own reply.
    void close_with(const std::string& reply);

    // A bound ended the turn. The reply is the stop reason the host sees.
    void stop_with(const std::string& reply);

    bool has_reply() const noexcept { return !final_reply_.empty(); }
    std::string take_reply() noexcept { return std::move(final_reply_); }
    bool closed_by_model() const noexcept { return model_closed_; }
    bool ran_tools() const noexcept { return had_tool_calls_; }
    const std::string& last_failure() const noexcept { return last_failure_; }

private:
    FailStreak fail_streak_;
    int loop_count_ = 0;
    int text_loop_count_ = 0;
    int tool_recovery_attempts_ = 0;
    int internal_failures_ = 0;
    int silence_ = 0;
    int notices_ = 0;
    int repeat_count_ = 0;
    int noted_repeats_ = 0;
    bool model_closed_ = false;
    bool budget_noted_ = false;
    bool wall_noted_ = false;
    bool had_tool_calls_ = false;
    std::string last_failure_;
    std::string last_call_key_;
    std::string last_loop_key_;
    std::string last_text_;
    std::string final_reply_;
    std::chrono::steady_clock::time_point start_;
    std::chrono::steady_clock::time_point deadline_;
};

} // namespace agent

#endif // AGENT_TURN_H
