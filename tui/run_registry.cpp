#include "tui/run_registry.h"

#include <vector>

namespace tui {

RunSlot& RunRegistry::slot(size_t window_id) {
    std::scoped_lock lk(map_mtx_);
    auto& p = slots_[window_id];
    if (!p)
        p = std::make_unique<RunSlot>();
    return *p;
}

bool RunRegistry::busy(size_t window_id) const {
    std::scoped_lock lk(map_mtx_);
    auto it = slots_.find(window_id);
    return it != slots_.end() && it->second->busy.load();
}

bool RunRegistry::any_busy() const {
    std::scoped_lock lk(map_mtx_);
    for (const auto& kv : slots_)
        if (kv.second->busy.load())
            return true;
    return false;
}

size_t RunRegistry::busy_count() const {
    std::scoped_lock lk(map_mtx_);
    size_t n = 0;
    for (const auto& kv : slots_)
        if (kv.second->busy.load())
            ++n;
    return n;
}

void RunRegistry::request_cancel(size_t window_id) {
    // Create the slot if needed: a cancel that arrives before the window's
    // first dispatch must still be visible to the worker when it starts.
    slot(window_id).cancel.store(true);
}

bool RunRegistry::cancelled(size_t window_id) const {
    std::scoped_lock lk(map_mtx_);
    auto it = slots_.find(window_id);
    return it != slots_.end() && it->second->cancel.load();
}

void RunRegistry::clear_cancel(size_t window_id) {
    std::scoped_lock lk(map_mtx_);
    auto it = slots_.find(window_id);
    if (it != slots_.end())
        it->second->cancel.store(false);
}

void RunRegistry::join(size_t window_id) {
    // Join outside the map lock: a finishing worker's last cancel check
    // needs map_mtx_, so holding it across join() deadlocks the caller.
    std::thread t;
    {
        std::scoped_lock lk(map_mtx_);
        auto it = slots_.find(window_id);
        if (it != slots_.end() && it->second->thread.joinable())
            t = std::move(it->second->thread);
    }
    if (t.joinable())
        t.join();
}

void RunRegistry::cancel_all() {
    std::scoped_lock lk(map_mtx_);
    for (const auto& kv : slots_)
        kv.second->cancel.store(true);
}

void RunRegistry::join_all() {
    // Same rule as join(): workers must be able to finish their last cancel
    // check while we wait — holding map_mtx_ across join() deadlocks
    // quit-while-busy.
    std::vector<std::thread> threads;
    {
        std::scoped_lock lk(map_mtx_);
        for (auto& kv : slots_)
            if (kv.second->thread.joinable())
                threads.push_back(std::move(kv.second->thread));
    }
    for (auto& t : threads)
        t.join();
}

void RunRegistry::enqueue(size_t window_id, std::string prompt) {
    RunSlot& s = slot(window_id);
    std::scoped_lock lk(s.mtx);
    s.pending.push_back(std::move(prompt));
}

std::optional<std::string> RunRegistry::pop_pending(size_t window_id) {
    std::scoped_lock lk(map_mtx_);
    auto it = slots_.find(window_id);
    if (it == slots_.end())
        return std::nullopt;
    RunSlot& s = *it->second;
    std::scoped_lock slk(s.mtx);
    if (s.pending.empty())
        return std::nullopt;
    std::string out = std::move(s.pending.front());
    s.pending.pop_front();
    return out;
}

bool RunRegistry::has_pending(size_t window_id) const {
    std::scoped_lock lk(map_mtx_);
    auto it = slots_.find(window_id);
    if (it == slots_.end())
        return false;
    std::scoped_lock slk(it->second->mtx);
    return !it->second->pending.empty();
}

void RunRegistry::erase(size_t window_id) {
    // busy==false does not mean the thread has returned — the worker clears
    // its flag as its last slot access, then unwinds. A joinable thread here
    // is finishing up; join it before the slot (and its std::thread member)
    // is destroyed, or std::thread's destructor calls std::terminate. The
    // join happens outside the map lock for the same reason as join_all.
    std::thread t;
    {
        std::scoped_lock lk(map_mtx_);
        auto it = slots_.find(window_id);
        if (it == slots_.end())
            return;
        if (it->second->thread.joinable())
            t = std::move(it->second->thread);
    }
    if (t.joinable())
        t.join();
    std::scoped_lock lk(map_mtx_);
    slots_.erase(window_id);
}

} // namespace tui
