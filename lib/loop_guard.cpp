#include "agent/loop_guard.h"

#include "agent/agent_helpers.h" // fingerprint_tool_calls
#include "agent/config.h"
#include "agent/context.h"
#include "agent/turn.h"
#include "agent/tool_recovery.h"

#include <chrono>
#include <utility>

namespace agent {

namespace {

// The one corrective note a repeated narration earns: the model gets the next
// word, not a dead turn.
constexpr char kTextSteer[] =
    "[harness] the same reply has arrived twice in a row. Either close the "
    "turn with the result, or take a different step — repeating the same "
    "words again changes nothing.";

} // namespace

LoopGuard::LoopGuard(const Config& cfg, const AgentHooks& hooks, ConversationLog& log,
                     Context& context, ContextEventSource& events, NoticeSink notice)
    : cfg_(cfg), hooks_(hooks), log_(log), context_(context), events_(events),
      notice_(std::move(notice)) {}

void LoopGuard::announce_dispatch(const json& calls, const std::string& content) const {
    if (calls.is_null() || calls.empty())
        return;
    if (hooks_.on_assistant && !content.empty())
        hooks_.on_assistant(content);
    if (hooks_.on_state)
        hooks_.on_state(RunState::Tooling);
    if (hooks_.on_debug)
        hooks_.on_debug("dispatching " + std::to_string(calls.size()) + " tool call(s)");
}

void LoopGuard::note_state(Turn& turn, int iteration) {
    if (turn.note_iteration_budget(iteration, cfg_.max_tool_iterations)) {
        notice_("[harness] step " + std::to_string(iteration + 1) + "/" +
                std::to_string(cfg_.max_tool_iterations) +
                " of this turn's iteration budget is used.");
    }
    if (turn.note_wall_budget(cfg_.max_wall_ms, std::chrono::steady_clock::now())) {
        const long wall_ms = turn.elapsed_ms(std::chrono::steady_clock::now());
        notice_("[harness] " + std::to_string(wall_ms / 60000) +
                "m of this turn's wall-clock budget is used.");
    }
    const int repeated = turn.note_repeated_call();
    if (repeated > 0) {
        notice_("[harness] the same tool call has been issued " + std::to_string(repeated) +
                " times in a row; arguments that only change the timing do not count.");
    }
}

bool LoopGuard::stop_dispatch_loop(const json& calls, Turn& turn) {
    if (!cfg_.detection_loop)
        return false;
    if (!turn.record_dispatch_fingerprint(fingerprint_tool_calls(calls)))
        return false;
    if (hooks_.on_status)
        hooks_.on_status("loop detected: breaking tool loop");
    log_.event("error", {{"reason", "tool_loop_detected"}});
    turn.stop_with("[loop detected: the model repeated the same tool "
                   "call 3+ times. Rephrase or break down the task.]");
    return true;
}

bool LoopGuard::repeated_narration(const std::string& content, Turn& turn) {
    if (!cfg_.detection_loop)
        return false;
    const TextRepeat repeat = turn.observe_text(content);
    if (repeat == TextRepeat::Steer) {
        Message steer;
        steer.role = "user";
        steer.harness = true;
        steer.content = kTextSteer;
        context_.push(std::move(steer));
        events_.publish(context_.token_count(), context_.size());
        if (hooks_.on_status)
            hooks_.on_status("text loop: injected recovery steer");
        return false;
    }
    if (repeat != TextRepeat::Unrecoverable)
        return false;
    if (hooks_.on_status)
        hooks_.on_status("agent looped beyond recovery, stopping");
    log_.event("error", {{"reason", "text_loop_unrecoverable"}});
    constexpr char kStop[] = "[loop detected: the model repeated itself "
                             "and did not recover. Please rephrase your request.]";
    turn.stop_with(kStop);
    if (hooks_.on_assistant)
        hooks_.on_assistant(kStop);
    return true;
}

void LoopGuard::fold_fail_streak(const json& calls, bool dispatch_ok, Turn& turn) {
    if (!cfg_.detection_loop)
        return;
    if (turn.record_tool_result(calls, dispatch_ok) < Turn::kFailStreakAt)
        return;
    if (turn.claim_recovery_steer()) {
        inject_tool_recovery_steer(&context_, hooks_, log_);
        return;
    }
    if (hooks_.on_status)
        hooks_.on_status("tool recovery failed, stopping");
    log_.event("tool_recovery", {{"action", "hard_stop"}});
    turn.stop_with("[stopped: tool calls kept failing after recovery "
                   "steer; rephrase your request or run a simpler command]");
}

} // namespace agent
