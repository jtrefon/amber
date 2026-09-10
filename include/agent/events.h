#ifndef AGENT_EVENTS_H
#define AGENT_EVENTS_H

// Typed event layer over EventBus (spec §6).
//
// Payload structs are the plugin-facing contract: subscribers receive the
// concrete type, never a void* to cast. EventBus stays the primitive — its
// dispatch semantics are unchanged and tested — and this layer maps payloads
// onto its event ids.
//
// Performance: publish() checks a per-type subscriber count with one atomic
// load and returns before touching the bus when nobody is listening. Nothing
// publishes per token; streaming stays an AgentHooks/UI concern.

#include "agent/event_bus.h"
#include "agent/llm.h"
#include "agent/tool.h"

#include <cstddef>
#include <functional>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace agent {

// --- Payload catalogue -----------------------------------------------------
// Pointers in payloads are owned by the publisher and valid only for the
// duration of the call.

struct TurnStartedEvent {
    std::string prompt;   // interceptable: an interceptor may rewrite it
};

struct TurnEndedEvent {
    bool cancelled = false;
};

struct MessageAddedEvent {
    const Message* message = nullptr;
    std::size_t index = 0;
};

struct ToolRequestedEvent {
    std::string name;
    json args;            // interceptable: an interceptor may rewrite them
    bool cancel = false;  // set by an interceptor to block execution
};

struct ToolCompletedEvent {
    std::string name;
    const ToolResult* result = nullptr;
    long duration_ms = 0;
};

struct LlmResponseEvent {
    long status = 0;
    long prompt_tokens = -1;
    long completion_tokens = -1;
};

struct CompressionEvent {
    long tokens = 0;
    long budget = 0;
    double threshold = 0.0;
};

struct CompressionCompletedEvent {
    bool success = false;
    long tokens_after = 0;
};

struct ErrorRaisedEvent {
    std::string kind;     // "transport", "auth", "overflow", ...
    std::string message;
    bool retryable = false;
};

struct PluginLoadedEvent {
    std::string plugin_id;
};

struct PluginUnloadedEvent {
    std::string plugin_id;
};

// --- Payload to bus id -----------------------------------------------------

template <class E>
struct EventTraits;

template <> struct EventTraits<TurnStartedEvent> {
    static constexpr EventType type = EventType::AgentTurnStart;
};
template <> struct EventTraits<TurnEndedEvent> {
    static constexpr EventType type = EventType::AgentTurnEnd;
};
template <> struct EventTraits<MessageAddedEvent> {
    static constexpr EventType type = EventType::MessageAdded;
};
template <> struct EventTraits<ToolRequestedEvent> {
    static constexpr EventType type = EventType::ToolCallBefore;
};
template <> struct EventTraits<ToolCompletedEvent> {
    static constexpr EventType type = EventType::ToolCallAfter;
};
template <> struct EventTraits<LlmResponseEvent> {
    static constexpr EventType type = EventType::LLMResponseAfter;
};
template <> struct EventTraits<CompressionEvent> {
    static constexpr EventType type = EventType::CompressionTriggered;
};
template <> struct EventTraits<CompressionCompletedEvent> {
    static constexpr EventType type = EventType::CompressionCompleted;
};
template <> struct EventTraits<ErrorRaisedEvent> {
    static constexpr EventType type = EventType::ErrorRaised;
};
template <> struct EventTraits<PluginLoadedEvent> {
    static constexpr EventType type = EventType::PluginLoaded;
};
template <> struct EventTraits<PluginUnloadedEvent> {
    static constexpr EventType type = EventType::PluginUnloaded;
};

// --- Subscription ----------------------------------------------------------
// RAII: dropping the handle removes the subscription, so a deactivated plugin
// cannot leave a callback behind.

class Subscription {
public:
    Subscription() = default;
    Subscription(EventBus* bus, std::size_t id) noexcept : bus_(bus), id_(id) {}
    ~Subscription() { release(); }

    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    Subscription(Subscription&& other) noexcept
        : bus_(other.bus_), id_(other.id_) {
        other.bus_ = nullptr;
    }
    Subscription& operator=(Subscription&& other) noexcept {
        if (this != &other) {
            release();
            bus_ = other.bus_;
            id_ = other.id_;
            other.bus_ = nullptr;
        }
        return *this;
    }

    void release() noexcept {
        if (bus_) {
            bus_->unsubscribe(id_);
            bus_ = nullptr;
        }
    }

private:
    EventBus* bus_ = nullptr;
    std::size_t id_ = 0;
};

// --- Typed facade ----------------------------------------------------------

class Events {
public:
    explicit Events(EventBus& bus) noexcept : bus_(bus) {}

    template <class E>
    Subscription subscribe(std::function<void(const E&)> handler) {
        return Subscription(&bus_,
                            bus_.subscribe(EventTraits<E>::type,
                                           [h = std::move(handler)](const Event& ev) {
                                               if (ev.data) h(*static_cast<const E*>(ev.data));
                                           }));
    }

    // Interceptors run before observers, newest first; returning false cancels
    // the event, so nothing downstream (and no observer) sees it.
    template <class E>
    Subscription intercept(std::function<bool(E&)> handler) {
        return Subscription(&bus_,
                            bus_.intercept(EventTraits<E>::type,
                                           [h = std::move(handler)](Event& ev) -> bool {
                                               if (!ev.data) return true;
                                               bool keep = h(*static_cast<E*>(ev.data));
                                               if (!keep) ev.cancelled = true;
                                               return keep;
                                           }));
    }

    // Returns false when an interceptor cancelled the event. `e` may be
    // modified by interceptors, so callers should read it back afterwards.
    template <class E>
    bool publish(E& e) {
        if (!bus_.has_subscribers(EventTraits<E>::type)) return true;
        Event ev{EventTraits<E>::type, &e, false};
        return bus_.fire(EventTraits<E>::type, ev);
    }

private:
    EventBus& bus_;
};

} // namespace agent

#endif // AGENT_EVENTS_H
