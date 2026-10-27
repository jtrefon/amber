#ifndef BENCH_SHAPE_REPLAY_H
#define BENCH_SHAPE_REPLAY_H

#include <cstddef>
#include <string>
#include <vector>

#include "agent/llm.h"

namespace bench {

// A recorded session replayed as the history the model saw: user/assistant
// messages, plus each tool call and the envelope its result produced.
struct ReplayLog {
    std::vector<agent::Message> messages;
};

struct ShapeStats {
    std::string rule;
    std::size_t messages = 0;
    std::size_t tokens_before = 0;
    std::size_t tokens_after = 0;
    std::size_t replaced = 0;
    // Replaced messages whose content a later message names (a path, a line, a
    // value). Mechanical, not semantic: it is the lower bound on "dropped but
    // still needed".
    std::size_t referenced = 0;
};

// Read a conversation log and rebuild the history it describes. Tool results
// are re-enveloped with the default budget, because the log stores the raw
// output while the model saw the envelope.
ReplayLog parse_session_log(const std::string& path);

// Apply one shaper to the replay and report what it cost and what it lost.
// Rules: "in-turn" (shipped, CC-14), "bash" and "supersede" (candidates under
// evaluation), "drop-all" (deliberately wrong, so the reference detector can be
// tested against a shaper that must fail).
ShapeStats replay_rule(const ReplayLog& log, const std::string& rule, std::size_t window);

// One aligned table row per rule, plus a totals line.
std::string format_replay_report(const std::string& path, const std::vector<ShapeStats>& stats);

} // namespace bench

#endif // BENCH_SHAPE_REPLAY_H
