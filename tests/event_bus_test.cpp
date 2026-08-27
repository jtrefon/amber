
#include "agent/event_bus.h"
#include "test_util.h"
#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

using namespace agent;

TEST(event_bus_subscribe_and_fire) {
    EventBus bus;
    int count = 0;
    bus.subscribe(EventType::AgentTurnStart, [&](const Event&) { ++count; });

    Event e{EventType::AgentTurnStart, nullptr};
    bus.fire(EventType::AgentTurnStart, e);
    ASSERT_EQ(count, 1);
}

TEST(event_bus_multiple_observers) {
    EventBus bus;
    int a = 0, b = 0;
    bus.subscribe(EventType::ToolCallAfter, [&](const Event&) { ++a; });
    bus.subscribe(EventType::ToolCallAfter, [&](const Event&) { ++b; });

    Event e{EventType::ToolCallAfter, nullptr};
    bus.fire(EventType::ToolCallAfter, e);
    ASSERT_EQ(a, 1);
    ASSERT_EQ(b, 1);
}

TEST(event_bus_unsubscribe) {
    EventBus bus;
    int count = 0;
    size_t id = bus.subscribe(EventType::AgentTurnEnd, [&](const Event&) { ++count; });

    Event e{EventType::AgentTurnEnd, nullptr};
    bus.fire(EventType::AgentTurnEnd, e);
    ASSERT_EQ(count, 1);

    bus.unsubscribe(id);
    bus.fire(EventType::AgentTurnEnd, e);
    ASSERT_EQ(count, 1);
}

TEST(event_bus_intercept_modifies) {
    EventBus bus;
    int value = 0;

    bus.intercept(EventType::AgentTurnStart, [&](Event&) -> bool {
        value = 42;
        return true;
    });

    Event e{EventType::AgentTurnStart, nullptr};
    bus.fire(EventType::AgentTurnStart, e);
    ASSERT_EQ(value, 42);
}

TEST(event_bus_intercept_cancels) {
    EventBus bus;
    int observer_count = 0;
    bool interceptor_ran = false;

    bus.intercept(EventType::ToolCallBefore, [&](Event& e) -> bool {
        interceptor_ran = true;
        e.cancelled = true;
        return false;
    });
    bus.subscribe(EventType::ToolCallBefore, [&](const Event&) { ++observer_count; });

    Event e{EventType::ToolCallBefore, nullptr};
    bool continued = bus.fire(EventType::ToolCallBefore, e);
    ASSERT_FALSE(continued);
    ASSERT_TRUE(interceptor_ran);
    ASSERT_EQ(observer_count, 0);
}

TEST(event_bus_intercept_order) {
    EventBus bus;
    std::vector<int> order;

    bus.intercept(EventType::AgentTurnStart, [&](Event&) -> bool {
        order.push_back(1);
        return true;
    });
    bus.intercept(EventType::AgentTurnStart, [&](Event&) -> bool {
        order.push_back(2);
        return true;
    });

    Event e{EventType::AgentTurnStart, nullptr};
    bus.fire(EventType::AgentTurnStart, e);
    ASSERT_EQ(order.size(), 2u);
    ASSERT_EQ(order[0], 2);
    ASSERT_EQ(order[1], 1);
}

TEST(event_bus_clear_removes_all) {
    EventBus bus;
    int count = 0;
    bus.subscribe(EventType::AgentTurnEnd, [&](const Event&) { ++count; });
    bus.intercept(EventType::AgentTurnEnd, [&](Event&) -> bool {
        ++count;
        return true;
    });

    bus.clear();
    Event e{EventType::AgentTurnEnd, nullptr};
    bus.fire(EventType::AgentTurnEnd, e);
    ASSERT_EQ(count, 0);
}

TEST(event_bus_different_types_independent) {
    EventBus bus;
    int start_count = 0, end_count = 0;
    bus.subscribe(EventType::AgentTurnStart, [&](const Event&) { ++start_count; });
    bus.subscribe(EventType::AgentTurnEnd, [&](const Event&) { ++end_count; });

    Event e1{EventType::AgentTurnStart, nullptr};
    bus.fire(EventType::AgentTurnStart, e1);
    ASSERT_EQ(start_count, 1);
    ASSERT_EQ(end_count, 0);

    Event e2{EventType::AgentTurnEnd, nullptr};
    bus.fire(EventType::AgentTurnEnd, e2);
    ASSERT_EQ(start_count, 1);
    ASSERT_EQ(end_count, 1);
}

TEST(event_bus_fire_reentrancy_subscribe_inside_handler) {
    auto bus = std::make_shared<EventBus>();
    bus->subscribe(EventType::AgentTurnStart, [bus](const Event&) {
        bus->subscribe(EventType::AgentTurnStart, [](const Event&) {});
    });
    Event e{EventType::AgentTurnStart, nullptr};
    std::atomic<bool> fired{false};
    std::thread fire_thread([bus, e, &fired]() mutable {
        bus->fire(EventType::AgentTurnStart, e);
        fired = true;
    });
    fire_thread.detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    ASSERT(fired.load());
}

TEST(event_bus_fire_reentrancy_fire_inside_handler) {
    auto bus = std::make_shared<EventBus>();
    std::atomic<int> outer{0}, inner{0};
    bus->subscribe(EventType::AgentTurnEnd, [bus, &inner](const Event&) { ++inner; });
    bus->subscribe(EventType::AgentTurnStart, [bus, &outer](const Event&) {
        ++outer;
        Event e2{EventType::AgentTurnEnd, nullptr};
        bus->fire(EventType::AgentTurnEnd, e2);
    });
    Event e{EventType::AgentTurnStart, nullptr};
    std::atomic<bool> done{false};
    std::thread t([bus, e, &done]() mutable {
        bus->fire(EventType::AgentTurnStart, e);
        done = true;
    });
    t.detach();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    ASSERT(done.load());
    ASSERT_EQ(outer.load(), 1);
    ASSERT_EQ(inner.load(), 1);
}


