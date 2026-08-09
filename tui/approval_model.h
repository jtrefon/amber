#ifndef AMBER_TUI_APPROVAL_MODEL_H
#define AMBER_TUI_APPROVAL_MODEL_H

#include <agent/agent.h>

#include <chrono>
#include <cmath>
#include <functional>

namespace tui {

// Pure decision logic for the approval dialog: countdown, selection, and
// verdict resolution. No ncurses, no I/O — the dialog widget pumps keys and
// time into the model and renders its state. Fail-safe rule: when the
// countdown elapses the dialog DENIES; it never auto-confirms the default
// selection.
class ApprovalModel {
public:
    using Clock = std::function<double()>;

    // Monotonic seconds; the default clock for production use.
    static double steady_seconds() {
        return std::chrono::duration<double>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    // timeout_sec <= 0 waits indefinitely (no countdown).
    ApprovalModel(int timeout_sec, int default_idx, Clock clock = steady_seconds)
        : clock_(std::move(clock)),
          timeout_sec_(timeout_sec),
          deadline_(timeout_sec_ > 0 ? clock_() + timeout_sec_ : 0.0),
          sel_(default_idx < 0 ? 0 : (default_idx > 3 ? 3 : default_idx)) {}

    // Advance time; sets timed_out() when the deadline passes.
    void poll() noexcept {
        if (timed_out_ || timeout_sec_ <= 0) return;
        if (clock_() >= deadline_) timed_out_ = true;
    }

    bool timed_out() const noexcept { return timed_out_; }

    int selection() const noexcept { return sel_; }

    void select(int idx) noexcept {
        sel_ = idx < 0 ? 0 : (idx > 3 ? 3 : idx);
    }

    int remaining_sec() const noexcept {
        if (timeout_sec_ <= 0 || timed_out_) return 0;
        double rem = deadline_ - clock_();
        // Round up so the countdown shows 1s until the deadline actually
        // passes (truncation showed 0s a fraction early and the timeout
        // fired while the display still said 0).
        return rem <= 0 ? 0 : static_cast<int>(std::ceil(rem));
    }

    // Final verdict for a closed dialog; sel < 0 means cancel/Esc → Deny. A
    // timed-out dialog always DENIES regardless of the selection — the
    // fail-safe is a model invariant, not panel behavior that could regress.
    agent::Approval resolve(int sel) const noexcept {
        if (timed_out_ || sel < 0 || sel > 3) return agent::Approval::Deny;
        static const agent::Approval map[] = {
            agent::Approval::AllowOnce,
            agent::Approval::AllowSession,
            agent::Approval::AlwaysAllow,
            agent::Approval::AlwaysDeny,
        };
        return map[sel];
    }

private:
    Clock clock_;
    int timeout_sec_;
    double deadline_;
    int sel_;
    bool timed_out_ = false;
};

} // namespace tui

#endif // AMBER_TUI_APPROVAL_MODEL_H
