#include "tui/run_registry.h"

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
    std::scoped_lock lk(map_mtx_);
    auto it = slots_.find(window_id);
    if (it != slots_.end() && it->second->thread.joinable())
        it->second->thread.join();
}

void RunRegistry::cancel_all() {
    std::scoped_lock lk(map_mtx_);
    for (const auto& kv : slots_)
        kv.second->cancel.store(true);
}

void RunRegistry::join_all() {
    std::scoped_lock lk(map_mtx_);
    for (const auto& kv : slots_)
        if (kv.second->thread.joinable())
            kv.second->thread.join();
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
    std::scoped_lock lk(map_mtx_);
    auto it = slots_.find(window_id);
    if (it == slots_.end())
        return;
    // busy==false does not mean the thread has returned — the worker clears
    // its flag as its last slot access, then unwinds. A joinable thread here
    // is finishing up; join it before the slot (and its std::thread member)
    // is destroyed, or std::thread's destructor calls std::terminate.
    if (it->second->thread.joinable())
        it->second->thread.join();
    slots_.erase(it);
}

} // namespace tui
