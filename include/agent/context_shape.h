#ifndef AGENT_CONTEXT_SHAPE_H
#define AGENT_CONTEXT_SHAPE_H

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace agent {

struct Message;

// What the model may ask for. It names evidence, never an edit: the harness
// validates the plan against the live history and applies what is still there.
enum class ShapeOp { Drop, Superseded };

struct ShapePlan {
    ShapeOp op = ShapeOp::Drop;
    std::vector<std::string> result_ids;
    std::string reason;
};

struct ShapeOutcome {
    std::size_t applied = 0;     // messages replaced by a digest
    std::size_t freed_bytes = 0; // content the rebuild actually removes
    std::vector<std::string> rejected; // ids that matched no live result
};

// Indices of `read` results superseded by a later read of the same path. The
// agent can ask for this set without naming ids; it is the CC-18 rule with a
// trigger (a request) instead of a timer.
std::vector<std::size_t> superseded_read_candidates(const std::vector<Message>& history);

// Apply `plan`. Every replacement goes through the compaction digest path, so
// the payload stays recallable through read_result. Idempotent: a digest is
// never re-digested.
ShapeOutcome apply_shape_plan(std::vector<Message>& history, const ShapePlan& plan);

// Plans submitted by the tool between two request boundaries. Drained by the
// agent at the top of the next request; the tool never touches the context.
class ShapeQueue {
public:
    void push(ShapePlan plan);
    std::vector<ShapePlan> drain();
    bool empty() const;

private:
    mutable std::mutex mtx_;
    std::vector<ShapePlan> plans_;
};

} // namespace agent

#endif // AGENT_CONTEXT_SHAPE_H
