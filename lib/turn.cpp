#include "agent/turn.h"

namespace agent {

Turn::Turn(std::chrono::steady_clock::time_point start, int max_wall_ms)
    : start_(start), deadline_(max_wall_ms > 0 ? start + std::chrono::milliseconds(max_wall_ms)
                                               : std::chrono::steady_clock::time_point::max()) {}

void Turn::record_tool_reply() {
    silence_ = 0;
    notices_ = 0;
    had_tool_calls_ = true;
}

TextRepeat Turn::observe_text(const std::string& content) {
    if (content.empty() || content != last_text_) {
        text_loop_count_ = 0;
        last_text_ = content;
        return TextRepeat::None;
    }
    ++text_loop_count_;
    if (text_loop_count_ >= kTextStopAt)
        return TextRepeat::Unrecoverable;
    return text_loop_count_ == kTextSteerAt ? TextRepeat::Steer : TextRepeat::None;
}

int Turn::observe_repeated_call(const std::string& canonical_key) {
    if (!canonical_key.empty() && canonical_key == last_call_key_) {
        ++repeat_count_;
    } else {
        repeat_count_ = 0;
        last_call_key_ = canonical_key;
    }
    return repeat_count_;
}

bool Turn::record_dispatch_fingerprint(const std::string& fingerprint) {
    if (!fingerprint.empty() && fingerprint == last_loop_key_) {
        ++loop_count_;
    } else {
        loop_count_ = 0;
        last_loop_key_ = fingerprint;
    }
    return loop_count_ >= kDispatchLoopAt;
}

int Turn::record_tool_result(const json& tool_calls, bool all_ok) {
    return fail_streak_.update(tool_calls, all_ok);
}

bool Turn::claim_recovery_steer() {
    if (tool_recovery_attempts_ >= 1)
        return false;
    ++tool_recovery_attempts_;
    return true;
}

bool Turn::record_notice() {
    silence_ = 0;
    return ++notices_ >= kMaxNotices;
}

bool Turn::record_silent_reply() {
    notices_ = 0;
    return ++silence_ >= kMaxSilence;
}

bool Turn::record_internal_failure() {
    return ++internal_failures_ >= kMaxInternalFailures;
}

bool Turn::wall_clock_exceeded(std::chrono::steady_clock::time_point now) const {
    return now >= deadline_;
}

long Turn::elapsed_ms(std::chrono::steady_clock::time_point now) const {
    return static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - start_).count());
}

bool Turn::note_iteration_budget(int iteration, int max_iterations) {
    if (budget_noted_ || max_iterations <= 0)
        return false;
    if (static_cast<long>(iteration + 1) * 100 < static_cast<long>(max_iterations) * kBudgetNotePct)
        return false;
    budget_noted_ = true;
    return true;
}

bool Turn::note_wall_budget(int max_wall_ms, std::chrono::steady_clock::time_point now) {
    if (wall_noted_ || max_wall_ms <= 0)
        return false;
    if (elapsed_ms(now) * 100 < static_cast<long>(max_wall_ms) * kBudgetNotePct)
        return false;
    wall_noted_ = true;
    return true;
}

int Turn::note_repeated_call() {
    if (repeat_count_ < kRepeatNoteAt || noted_repeats_ == repeat_count_)
        return 0;
    noted_repeats_ = repeat_count_;
    return repeat_count_ + 1;
}

void Turn::record_failure(const std::string& failure) {
    last_failure_ = failure;
}

void Turn::close_with(const std::string& reply) {
    silence_ = 0;
    notices_ = 0;
    model_closed_ = true;
    final_reply_ = reply;
}

void Turn::stop_with(const std::string& reply) {
    final_reply_ = reply;
}

} // namespace agent
