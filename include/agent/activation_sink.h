#ifndef AGENT_ACTIVATION_SINK_H
#define AGENT_ACTIVATION_SINK_H

#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "agent/skill_catalog.h" // ActivatedSkill

namespace agent {

// Session-activated skill bodies for ONE agent. The read_skill tool runs on a
// tool-dispatch worker, so this sink is reachable from concurrent threads and
// is internally synchronized. The dedup rule lives here, with the data it
// protects: activating a skill already active this session is a no-op.
class ActivationSink {
public:
    ActivationSink() = default;
    ActivationSink(const ActivationSink&) = delete;
    ActivationSink& operator=(const ActivationSink&) = delete;
    ~ActivationSink() = default;
    // Movable so an Agent stays movable (hosts move one into a unique_ptr).
    // Moves happen before the sink is shared with any thread, so locking the
    // source is enough.
    ActivationSink(ActivationSink&& other) noexcept { move_from(other); }
    ActivationSink& operator=(ActivationSink&& other) noexcept {
        if (this != &other)
            move_from(other);
        return *this;
    }

    void record(const std::string& name, const std::string& body) {
        std::scoped_lock lk(mtx_);
        for (const auto& a : items_)
            if (a.name == name)
                return;
        items_.push_back({name, body});
    }

    std::vector<ActivatedSkill> snapshot() const {
        std::scoped_lock lk(mtx_);
        return items_;
    }

    void assign(const std::vector<ActivatedSkill>& items) {
        std::scoped_lock lk(mtx_);
        items_ = items;
    }

private:
    void move_from(ActivationSink& other) {
        std::scoped_lock lk(other.mtx_);
        items_ = std::move(other.items_);
    }

    mutable std::mutex mtx_;
    std::vector<ActivatedSkill> items_;
};

} // namespace agent

#endif // AGENT_ACTIVATION_SINK_H
