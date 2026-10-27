#include "agent/context_shape.h"

#include "agent/llm.h"

namespace agent {

std::vector<std::size_t> superseded_read_candidates(const std::vector<Message>& history) {
    (void)history;
    return {};
}

ShapeOutcome apply_shape_plan(std::vector<Message>& history, const ShapePlan& plan) {
    (void)history;
    (void)plan;
    return {};
}

void ShapeQueue::push(ShapePlan plan) {
    std::scoped_lock lk(mtx_);
    plans_.push_back(std::move(plan));
}

std::vector<ShapePlan> ShapeQueue::drain() {
    std::scoped_lock lk(mtx_);
    std::vector<ShapePlan> out = std::move(plans_);
    plans_.clear();
    return out;
}

bool ShapeQueue::empty() const {
    std::scoped_lock lk(mtx_);
    return plans_.empty();
}

} // namespace agent
