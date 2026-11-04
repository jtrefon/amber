#include "agent/events.h"
#include "agent/task.h"
#include "agent/task_inbox.h"
#include "tests/test_util.h"

#include <string>
#include <vector>

namespace {

agent::TaskCompletion completion(const std::string& id, const std::string& owner,
                                 std::int64_t created_ms, const std::string& line = {}) {
    agent::TaskCompletion c;
    c.task_id = id;
    c.owner_id = owner;
    c.kind = agent::TaskKind::Command;
    c.state = agent::TaskState::Succeeded;
    c.prompt_text = line.empty() ? (id + " (command) succeeded") : line;
    c.created_ms = created_ms;
    return c;
}

} // namespace

TEST(inbox_take_returns_one_digest_for_many) {
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    agent::TaskInbox inbox(bus, tasks);
    for (int i = 0; i < 10; ++i)
        inbox.post(completion("task_" + std::to_string(i), "w", i));

    const auto digest = inbox.take("w");
    ASSERT_EQ(digest.size(), 10u);
    const std::string text = agent::format_task_digest(digest);
    ASSERT_TRUE(text.find("10 completed") != std::string::npos);
    ASSERT_TRUE(text.find("automated event") != std::string::npos);
    ASSERT_TRUE(inbox.take("w").empty());
    ASSERT_EQ(inbox.pending("w"), 10u);
}

TEST(inbox_take_sorts_by_completion_time) {
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    agent::TaskInbox inbox(bus, tasks);
    inbox.post(completion("late", "w", 30));
    inbox.post(completion("early", "w", 10));
    inbox.post(completion("middle", "w", 20));
    const auto digest = inbox.take("w");
    ASSERT_EQ(digest.size(), 3u);
    ASSERT_EQ(digest[0].task_id, std::string("early"));
    ASSERT_EQ(digest[1].task_id, std::string("middle"));
    ASSERT_EQ(digest[2].task_id, std::string("late"));
}

TEST(inbox_ack_is_idempotent) {
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    agent::TaskInbox inbox(bus, tasks);
    inbox.post(completion("one", "w", 1));
    const auto digest = inbox.take("w");
    ASSERT_EQ(digest.size(), 1u);
    inbox.ack("w", {"one"});
    ASSERT_EQ(inbox.pending("w"), 0u);
    ASSERT_TRUE(inbox.take("w").empty());
    inbox.post(completion("one", "w", 1));
    ASSERT_EQ(inbox.pending("w"), 0u);
}

TEST(inbox_owner_isolation) {
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    agent::TaskInbox inbox(bus, tasks);
    inbox.post(completion("a", "one", 1));
    inbox.post(completion("b", "two", 2));
    ASSERT_EQ(inbox.pending("one"), 1u);
    ASSERT_EQ(inbox.pending("two"), 1u);
    ASSERT_EQ(inbox.take("one").size(), 1u);
    ASSERT_EQ(inbox.pending("two"), 1u);
    ASSERT_TRUE(inbox.take("missing").empty());
}

TEST(inbox_subscribes_to_task_completed) {
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    agent::TaskInbox inbox(bus, tasks);
    agent::TaskRecord record;
    record.kind = agent::TaskKind::Command;
    record.owner.owner_id = "w";
    record.label = "build";
    record.created_ms = 5;
    const std::string id = tasks.create(record);
    ASSERT_TRUE(tasks.started(id, 6));
    ASSERT_TRUE(tasks.finish(id, agent::TaskState::Succeeded, {}));

    ASSERT_EQ(inbox.pending("w"), 1u);
    const auto digest = inbox.take("w");
    ASSERT_EQ(digest.size(), 1u);
    ASSERT_TRUE(digest[0].prompt_text.find("build") != std::string::npos);
    ASSERT_TRUE(digest[0].prompt_text.find("succeeded") != std::string::npos);
    inbox.ack("w", {id});
    ASSERT_EQ(inbox.pending("w"), 0u);
}

TEST(inbox_silent_is_not_delivered) {
    agent::EventBus bus;
    agent::TaskService tasks(bus);
    agent::TaskInbox inbox(bus, tasks);
    agent::TaskRecord record;
    record.owner.owner_id = "w";
    record.notify = agent::TaskNotify::Silent;
    const std::string id = tasks.create(record);
    ASSERT_TRUE(tasks.started(id, 1));
    ASSERT_TRUE(tasks.finish(id, agent::TaskState::Succeeded, {}));
    ASSERT_EQ(inbox.pending("w"), 0u);
}

TEST(format_digest_bounds_a_large_batch) {
    std::vector<agent::TaskCompletion> items;
    for (int i = 0; i < 100; ++i)
        items.push_back(completion("task_" + std::to_string(i), "w", i, std::string(512, 'x')));
    const std::string digest = agent::format_task_digest(items);
    ASSERT_TRUE(digest.size() <= 8 * 1024 + 128);
    ASSERT_TRUE(digest.find("more completions") != std::string::npos);
}
