
#include <string>

#include "agent/experience.h"
#include "agent/learn_commands.h"
#include "tests/test_util.h"

namespace {

agent::ExperienceConfig ec(const std::string& path) {
    agent::ExperienceConfig cfg;
    cfg.store_path = path;
    return cfg;
}

void add_memory(agent::MemoryStore& store, const std::string& name,
                const std::string& content, int evidence, bool promoted) {
    agent::Memory m;
    m.name = name;
    m.content = content;
    m.tags = {"build", "make"};
    m.evidence_count = evidence;
    m.promoted = promoted;
    m.last_confirm_turn = 12;
    store.upsert(m);
}

void add_skill(agent::MemoryStore& store, const std::string& name,
               const std::string& content, int evidence) {
    agent::Skill sk;
    sk.name = name;
    sk.content = content;
    sk.evidence_count = evidence;
    sk.last_confirm_turn = 3;
    sk.trigger_phrase = "run the tests";
    store.upsert(sk);
}

} // namespace

// [LU-01] Show lines follow the formatting contract, score-sorted.
TEST(learn_show_lines_format) {
    std::string path = "/tmp/amber_learn_cmd.json";
    std::remove(path.c_str());
    auto store = agent::make_memory_store(ec(path));
    store->set_current_turn(20);
    add_memory(*store, "project uses make", "uses make", 3, true);
    add_skill(*store, "run-tests", "run the suite", 5);

    auto lines = agent::learn_show_lines(store.get(), "");
    ASSERT_EQ(lines.size(), 2u);
    // score-sorted: skill (evidence 5) before memory (evidence 3)
    ASSERT(lines[0].find("run-tests") != std::string::npos);
    ASSERT(lines[0].find("skill") != std::string::npos);
    ASSERT(lines[0].find("evidence 5") != std::string::npos);
    ASSERT(lines[0].find("promoted") != std::string::npos);
    ASSERT(lines[0].find("trigger \"run the tests\"") != std::string::npos);
    ASSERT(lines[1].find("memory") != std::string::npos);
    ASSERT(lines[1].find("evidence 3") != std::string::npos);
    ASSERT(lines[1].find("turn 12") != std::string::npos);
    std::remove(path.c_str());
}

// [LU-06] Type and tag filters.
TEST(learn_show_lines_filter) {
    std::string path = "/tmp/amber_learn_cmd_filter.json";
    std::remove(path.c_str());
    auto store = agent::make_memory_store(ec(path));
    add_memory(*store, "uses make", "m", 2, false);
    add_skill(*store, "run-tests", "s", 2);

    ASSERT_EQ(agent::learn_show_lines(store.get(), "memory").size(), 1u);
    ASSERT_EQ(agent::learn_show_lines(store.get(), "skill").size(), 1u);
    ASSERT_EQ(agent::learn_show_lines(store.get(), "make").size(), 1u);
    ASSERT_EQ(agent::learn_show_lines(store.get(), "zzz").size(), 0u);
    std::remove(path.c_str());
}

// [LU-07] Empty store message.
TEST(learn_show_lines_empty) {
    std::string path = "/tmp/amber_learn_cmd_empty.json";
    std::remove(path.c_str());
    auto store = agent::make_memory_store(ec(path));
    auto lines = agent::learn_show_lines(store.get(), "");
    ASSERT_EQ(lines.size(), 1u);
    ASSERT_EQ(lines[0], "(no learned items)");
    std::remove(path.c_str());
}

// [LU-08] Null store reports the store as disabled.
TEST(learn_show_lines_disabled) {
    auto lines = agent::learn_show_lines(nullptr, "");
    ASSERT_EQ(lines.size(), 1u);
    ASSERT_EQ(lines[0], "experience store disabled");
}

// [LU-02] Inspect detail page.
TEST(learn_inspect_lines) {
    std::string path = "/tmp/amber_learn_cmd_inspect.json";
    std::remove(path.c_str());
    auto store = agent::make_memory_store(ec(path));
    store->set_current_turn(20);
    add_memory(*store, "uses make", "run make test", 2, true);
    std::string id = store->all_memories()[0].id;

    std::string err;
    auto lines = agent::learn_inspect_lines(store.get(), id, err);
    ASSERT_EQ(err, "");
    ASSERT_EQ(lines.size(), 8u);
    ASSERT_EQ(lines[0], "id: " + id);
    ASSERT_EQ(lines[1], "type: memory");
    ASSERT_EQ(lines[2], "name: uses make");
    ASSERT_EQ(lines[3], "content: run make test");
    ASSERT_EQ(lines[4], "tags: build, make");
    ASSERT_EQ(lines[5], "evidence: 2");
    ASSERT_EQ(lines[6], "promoted: yes");
    ASSERT_EQ(lines[7], "last turn: 12");

    auto lines2 = agent::learn_inspect_lines(store.get(), "zzz", err);
    ASSERT_FALSE(err.empty());
    ASSERT(lines2.empty());
    ASSERT(err.find("no learned item with id") != std::string::npos);
    std::remove(path.c_str());
}

// [LU-02] Skill inspect includes the trigger phrase.
TEST(learn_inspect_lines_skill) {
    std::string path = "/tmp/amber_learn_cmd_inspect_sk.json";
    std::remove(path.c_str());
    auto store = agent::make_memory_store(ec(path));
    add_skill(*store, "run-tests", "run the suite", 5);
    std::string id = store->all_skills()[0].id;

    std::string err;
    auto lines = agent::learn_inspect_lines(store.get(), id, err);
    ASSERT_EQ(err, "");
    ASSERT_EQ(lines[1], "type: skill");
    ASSERT_EQ(lines[8], "trigger: run the tests");
    std::remove(path.c_str());
}

// [LU-09] Budget summary.
TEST(learn_summary_lines) {
    std::string path = "/tmp/amber_learn_cmd_summary.json";
    std::remove(path.c_str());
    auto store = agent::make_memory_store(ec(path));
    add_memory(*store, "a", "x", 2, false);
    add_memory(*store, "b", "y", 3, true);
    add_skill(*store, "c", "z", 1);
    auto cfg = ec(path);
    cfg.max_memories = 20;
    cfg.max_skills = 10;

    auto lines = agent::learn_summary_lines(store.get(), cfg);
    ASSERT_EQ(lines.size(), 1u);
    ASSERT_EQ(lines[0],
              "memories: 2/20 \u00b7 skills: 1/10 \u00b7 promoted: 1 \u00b7 "
              "path: " + path);
    std::remove(path.c_str());
}
