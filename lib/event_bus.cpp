
#include "agent/event_bus.h"

#include <algorithm>

namespace agent {

namespace {

std::size_t type_index(EventType type) noexcept {
    return static_cast<std::size_t>(type);
}

} // namespace

EventBus::EventBus() noexcept {
    for (auto& count : counts_) count.store(0, std::memory_order_relaxed);
}

size_t EventBus::subscribe(EventType type, Observer handler) {
    std::scoped_lock lk(mtx_);
    size_t id = next_id_++;
    observers_.push_back({id, type, std::move(handler)});
    counts_[type_index(type)].fetch_add(1, std::memory_order_relaxed);
    return id;
}

size_t EventBus::intercept(EventType type, Interceptor handler) {
    std::scoped_lock lk(mtx_);
    size_t id = next_id_++;
    interceptors_.push_back({id, type, std::move(handler)});
    counts_[type_index(type)].fetch_add(1, std::memory_order_relaxed);
    return id;
}

bool EventBus::has_subscribers(EventType type) const noexcept {
    return counts_[type_index(type)].load(std::memory_order_relaxed) > 0;
}

bool EventBus::fire(EventType type, Event& event) {
    std::vector<InterceptorEntry> interceptors;
    std::vector<ObserverEntry> observers;
    {
        std::scoped_lock lk(mtx_);
        for (auto& e : interceptors_) {
            if (e.type == type) interceptors.push_back(e);
        }
        for (auto& e : observers_) {
            if (e.type == type) observers.push_back(e);
        }
    }
    for (auto it = interceptors.rbegin(); it != interceptors.rend(); ++it) {
        if (!it->handler(event)) return false;
    }
    for (auto& e : observers) {
        e.handler(event);
    }
    return true;
}

void EventBus::unsubscribe(size_t id) {
    std::scoped_lock lk(mtx_);
    for (auto it = observers_.begin(); it != observers_.end();) {
        if (it->id == id) {
            counts_[type_index(it->type)].fetch_sub(1, std::memory_order_relaxed);
            it = observers_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = interceptors_.begin(); it != interceptors_.end();) {
        if (it->id == id) {
            counts_[type_index(it->type)].fetch_sub(1, std::memory_order_relaxed);
            it = interceptors_.erase(it);
        } else {
            ++it;
        }
    }
}

void EventBus::clear() {
    std::scoped_lock lk(mtx_);
    observers_.clear();
    interceptors_.clear();
    for (auto& count : counts_) count.store(0, std::memory_order_relaxed);
}

} // namespace agent
