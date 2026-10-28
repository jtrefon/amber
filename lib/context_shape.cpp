#include "agent/context_shape.h"

#include <algorithm>
#include <map>

#include "agent/agent_helpers.h"
#include "agent/compressor.h"
#include "agent/llm.h"

namespace agent {
namespace {

bool is_digest(const Message& m) {
    return m.content.rfind("[tool output omitted", 0) == 0;
}

bool id_matches(const Message& m, const std::string& id) {
    return m.role == "tool" && !is_digest(m) && result_id_of(m) == id;
}

std::vector<std::size_t> live_id_targets(const std::vector<Message>& history,
                                         const std::vector<std::string>& ids,
                                         std::vector<std::string>& rejected) {
    std::vector<std::size_t> out;
    for (const std::string& id : ids) {
        bool live = false;
        for (std::size_t i = 0; i < history.size(); ++i) {
            if (!id_matches(history[i], id))
                continue;
            out.push_back(i);
            live = true;
        }
        if (!live)
            rejected.push_back(id);
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

std::vector<std::size_t> superseded_read_candidates(const std::vector<Message>& history) {
    std::map<std::string, std::vector<std::size_t>> reads;
    for (std::size_t i = 0; i < history.size(); ++i) {
        if (history[i].role != "tool" || history[i].name != "read")
            continue;
        const std::string path = tool_arg_of(history[i], "path");
        if (!path.empty())
            reads[path].push_back(i);
    }
    std::vector<std::size_t> out;
    for (const auto& entry : reads)
        for (std::size_t k = 0; k + 1 < entry.second.size(); ++k)
            out.push_back(entry.second[k]);
    std::sort(out.begin(), out.end());
    return out;
}

ShapeOutcome apply_shape_plan(std::vector<Message>& history, const ShapePlan& plan) {
    ShapeOutcome out;
    std::vector<std::size_t> targets =
        plan.op == ShapeOp::Superseded ? superseded_read_candidates(history)
                                       : live_id_targets(history, plan.result_ids, out.rejected);
    for (const std::size_t i : targets) {
        const std::size_t before = history[i].content.size();
        if (!digest_tool_message(history[i]))
            continue;
        out.freed_bytes += before - history[i].content.size();
        ++out.applied;
    }
    return out;
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
