
#include "agent/event_bus.h"

#include <algorithm>

namespace agent {

size_t EventBus::subscribe(EventType type, Observer handler) {
    std::scoped_lock lk(mtx_);
    size_t id = next_id_++;
    observers_.push_back({id, type, std::move(handler)});
    return id;
}

size_t EventBus::intercept(EventType type, Interceptor handler) {
    std::scoped_lock lk(mtx_);
    size_t id = next_id_++;
    interceptors_.push_back({id, type, std::move(handler)});
    return id;
}

bool EventBus::fire(EventType type, Event& event) {
    std::scoped_lock lk(mtx_);

    std::vector<InterceptorEntry*> matched;
    for (auto& e : interceptors_) {
        if (e.type == type) matched.push_back(&e);
    }
    for (auto it = matched.rbegin(); it != matched.rend(); ++it) {
        if (!(*it)->handler(event)) return false;
    }

    for (auto& e : observers_) {
        if (e.type == type) e.handler(event);
    }
    return true;
}

void EventBus::unsubscribe(size_t id) {
    std::scoped_lock lk(mtx_);
    observers_.erase(
        std::remove_if(observers_.begin(), observers_.end(),
                       [id](const ObserverEntry& e) { return e.id == id; }),
        observers_.end());
    interceptors_.erase(
        std::remove_if(interceptors_.begin(), interceptors_.end(),
                       [id](const InterceptorEntry& e) { return e.id == id; }),
        interceptors_.end());
}

void EventBus::clear() {
    std::scoped_lock lk(mtx_);
    observers_.clear();
    interceptors_.clear();
}

} // namespace agent
