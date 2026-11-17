// The turn loop's loop policy, exercised without an agent loop: repeated
// narration, a repeated dispatch fingerprint, a failing tool's streak, and the
// budget notices. The guard decides and reports; the agent still owns the
// request, the dispatcher and the conversation.

#include "agent.h"
#include "agent/context.h"
#include "agent/loop_guard.h"
#include "agent/turn.h"
#include "tests/test_util.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

using namespace agent;

namespace {

Config guard_cfg() {
    Config cfg;
    cfg.detection_loop = true;
    cfg.max_tool_iterations = 10;
    cfg.max_wall_ms = 60000;
    return cfg;
}

json bash_call(const std::string& command) {
    json calls = json::array();
    calls.push_back({{"id", "c1"},
                     {"type", "function"},
                     {"function", {{"name", "bash"}, {"arguments", {{"command", command}}}}}});
    return calls;
}

std::chrono::steady_clock::time_point now() {
    return std::chrono::steady_clock::now();
}

struct GuardFixture {
    Config cfg = guard_cfg();
    AgentHooks hooks;
    ConversationLog log;
    Context context;
    ContextEventSource events;
    std::vector<std::string> notes;
    std::vector<std::string> statuses;
    std::unique_ptr<LoopGuard> guard;

    GuardFixture() {
        hooks.on_status = [this](const std::string& line) { statuses.push_back(line); };
        guard = std::make_unique<LoopGuard>(cfg, hooks, log, context, events,
                                            [this](Message note) {
                                                notes.push_back(std::move(note.content));
                                            });
    }
};

bool notes_contain(const std::vector<std::string>& notes, const std::string& needle) {
    for (const auto& note : notes)
        if (note.find(needle) != std::string::npos)
            return true;
    return false;
}

} // namespace

// The same call three times in a row is a loop: the third attempt ends the
// turn instead of dispatching again. A changed call resets the run.
TEST(loop_guard_stops_a_dispatch_fingerprint_on_the_third_repeat) {
    GuardFixture f;
    Turn turn(now(), f.cfg.max_wall_ms);
    const json calls = bash_call("echo hi");

    ASSERT_FALSE(f.guard->stop_dispatch_loop(calls, turn));
    ASSERT_FALSE(f.guard->stop_dispatch_loop(calls, turn));
    ASSERT_TRUE(f.guard->stop_dispatch_loop(calls, turn));
    ASSERT(turn.has_reply());
    ASSERT(turn.take_reply().find("loop detected") != std::string::npos);
}

TEST(loop_guard_resets_the_dispatch_run_when_the_call_changes) {
    GuardFixture f;
    Turn turn(now(), f.cfg.max_wall_ms);
    const json first = bash_call("echo hi");
    const json second = bash_call("echo bye");

    ASSERT_FALSE(f.guard->stop_dispatch_loop(first, turn));
    ASSERT_FALSE(f.guard->stop_dispatch_loop(first, turn));
    ASSERT_FALSE(f.guard->stop_dispatch_loop(second, turn));
    ASSERT_FALSE(f.guard->stop_dispatch_loop(second, turn));
    ASSERT_TRUE(f.guard->stop_dispatch_loop(second, turn));
}

// Repeated narration earns one corrective note; the fifth repeat ends the turn
// instead of paying another request for the same words.
TEST(loop_guard_steers_once_when_narration_repeats_then_stops) {
    GuardFixture f;
    Turn turn(now(), f.cfg.max_wall_ms);

    ASSERT_FALSE(f.guard->repeated_narration("same words", turn));
    ASSERT_TRUE(f.notes.empty());
    ASSERT_FALSE(f.guard->repeated_narration("same words", turn));
    ASSERT_EQ(f.notes.size(), 1u);
    ASSERT(f.notes[0].find("same reply") != std::string::npos);
    ASSERT_FALSE(f.guard->repeated_narration("same words", turn));
    ASSERT_FALSE(f.guard->repeated_narration("same words", turn));
    ASSERT_TRUE(f.guard->repeated_narration("same words", turn));
    ASSERT(turn.has_reply());
}

// A failing tool steers the model off it once, then hard-stops. A successful
// batch clears the streak, so one success buys a fresh chance.
TEST(loop_guard_steers_off_a_failing_tool_then_stops) {
    GuardFixture f;
    Turn turn(now(), f.cfg.max_wall_ms);
    const json calls = bash_call("false");

    f.guard->fold_fail_streak(calls, false, turn);
    f.guard->fold_fail_streak(calls, false, turn);
    ASSERT_FALSE(turn.has_reply());
    ASSERT_TRUE(f.context.get_all().empty());

    f.guard->fold_fail_streak(calls, false, turn); // the steer
    ASSERT_FALSE(turn.has_reply());
    ASSERT_EQ(f.context.size(), 1u);
    ASSERT(f.context.get_all().front().content.find("keep failing") != std::string::npos);

    f.guard->fold_fail_streak(calls, false, turn); // the hard stop
    ASSERT(turn.has_reply());
    ASSERT(turn.take_reply().find("kept failing") != std::string::npos);

    GuardFixture fresh;
    Turn recovered(now(), fresh.cfg.max_wall_ms);
    fresh.guard->fold_fail_streak(calls, false, recovered);
    fresh.guard->fold_fail_streak(calls, false, recovered);
    fresh.guard->fold_fail_streak(calls, true, recovered); // success clears it
    fresh.guard->fold_fail_streak(calls, false, recovered);
    fresh.guard->fold_fail_streak(calls, false, recovered);
    ASSERT_FALSE(recovered.has_reply());
}

// The budget notices are derived from Turn and said once per turn, at the
// moment they become true: iteration budget, wall clock, and a repeated call.
TEST(loop_guard_notes_the_budgets_once_per_turn) {
    GuardFixture f;
    Turn turn(now() - std::chrono::seconds(50), f.cfg.max_wall_ms);

    f.guard->note_state(turn, f.cfg.max_tool_iterations - 1);
    ASSERT_EQ(f.notes.size(), 1u);
    ASSERT(f.notes[0].find("iteration budget") != std::string::npos);

    f.guard->note_state(turn, f.cfg.max_tool_iterations - 1);
    ASSERT_EQ(f.notes.size(), 1u); // once per turn

    Turn repeated(now(), f.cfg.max_wall_ms);
    repeated.observe_repeated_call("bash|echo hi");
    repeated.observe_repeated_call("bash|echo hi");
    repeated.observe_repeated_call("bash|echo hi");
    f.guard->note_state(repeated, 0);
    ASSERT(notes_contain(f.notes, "same tool call"));
}

// Wall-clock notice: the same guard, a turn that has spent 80% of its budget.
TEST(loop_guard_notes_the_wall_clock_once) {
    GuardFixture f;
    f.cfg.max_tool_iterations = 100; // keep the iteration note out of the way
    Turn turn(now() - std::chrono::seconds(50), f.cfg.max_wall_ms);

    f.guard->note_state(turn, 0);
    ASSERT_EQ(f.notes.size(), 1u);
    ASSERT(f.notes[0].find("wall-clock budget") != std::string::npos);
    f.guard->note_state(turn, 0);
    ASSERT_EQ(f.notes.size(), 1u);
}

// Detection off means the guard observes nothing and stops nothing. The
// budget notices are not detection, so they still fire.
TEST(loop_guard_detection_off_does_nothing) {
    GuardFixture f;
    f.cfg.detection_loop = false;
    Turn turn(now(), f.cfg.max_wall_ms);
    const json calls = bash_call("echo hi");

    for (int i = 0; i < 5; ++i)
        ASSERT_FALSE(f.guard->stop_dispatch_loop(calls, turn));
    for (int i = 0; i < 6; ++i)
        ASSERT_FALSE(f.guard->repeated_narration("same words", turn));
    f.guard->fold_fail_streak(calls, false, turn);
    f.guard->fold_fail_streak(calls, false, turn);
    f.guard->fold_fail_streak(calls, false, turn);
    f.guard->fold_fail_streak(calls, false, turn);
    ASSERT_FALSE(turn.has_reply());
    ASSERT(f.context.get_all().empty());

    f.guard->note_state(turn, f.cfg.max_tool_iterations - 1);
    ASSERT_EQ(f.notes.size(), 1u);
}

// The tooling-phase announcements ride the same dispatch event as before:
// narration once, state Tooling, and the call count for the debug log.
TEST(loop_guard_announces_the_dispatch) {
    GuardFixture f;
    std::string narration;
    RunState state = RunState::Idle;
    std::string debug;
    f.hooks.on_assistant = [&](const std::string& text) { narration = text; };
    f.hooks.on_state = [&](RunState value) { state = value; };
    f.hooks.on_debug = [&](const std::string& line) { debug = line; };

    f.guard->announce_dispatch(bash_call("echo hi"), "working");
    ASSERT_EQ(narration, std::string("working"));
    ASSERT(state == RunState::Tooling);
    ASSERT(debug.find("1 tool call") != std::string::npos);

    narration.clear();
    f.guard->announce_dispatch(bash_call("echo hi"), "");
    ASSERT(narration.empty());
}
