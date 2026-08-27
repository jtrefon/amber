
#ifndef AGENT_EVENT_BUS_H
#define AGENT_EVENT_BUS_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace agent {

enum class EventType : std::uint8_t {
    AgentTurnStart,
    AgentTurnEnd,
    ToolCallBefore,
    ToolCallAfter,
    MessageAdded,
    CompressionTriggered,
    LLMRequestBefore,
    LLMResponseAfter,
    TUIRender,
    TUIKeyPress,
    TUIInputChanged,
    PluginLoaded,
    PluginUnloaded,
};

struct Event {
    EventType type;
    void* data = nullptr;
    bool cancelled = false;
};

class EventBus {
public:
    using Observer = std::function<void(const Event&)>;
    using Interceptor = std::function<bool(Event&)>;

    size_t subscribe(EventType type, Observer handler);
    size_t intercept(EventType type, Interceptor handler);
    bool fire(EventType type, Event& event);
    void unsubscribe(size_t id);
    void clear();

private:
    struct ObserverEntry {
        size_t id;
        EventType type;
        Observer handler;
    };

    struct InterceptorEntry {
        size_t id;
        EventType type;
        Interceptor handler;
    };

    std::mutex mtx_;
    std::vector<ObserverEntry> observers_;
    std::vector<InterceptorEntry> interceptors_;
    std::atomic<size_t> next_id_{1};
};

} // namespace agent

#endif // AGENT_EVENT_BUS_H
