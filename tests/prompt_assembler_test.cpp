// Prompt assembly, exercised without the agent loop. The ordering contract is
// the one the KV prefix depends on, so it is pinned here on plain sources and
// then through the real loop in agent_loop_test.cpp.

#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "agent/activation_sink.h"
#include "agent/config.h"
#include "agent/experience.h"
#include "agent/extensions.h"
#include "agent/llm.h"
#include "agent/prompt_assembler.h"
#include "agent/session_brief.h"
#include "agent/skill_catalog.h"
#include "agent/tools.h"
#include "agent/workspace.h"
#include "tests/test_util.h"

using namespace agent;

namespace {

namespace fs = std::filesystem;

class NamedTool : public Tool {
public:
    explicit NamedTool(std::string name) : name_(std::move(name)) {}
    std::string name() const noexcept override { return name_; }
    std::string description() const noexcept override { return "test tool"; }
    json parameters_schema() const override { return json::object(); }
    ToolResult execute(const json&) const override { return {true, "ok", ""}; }

private:
    std::string name_;
};

// One authored skill in a scratch workspace, scanned into a real catalog.
class SkillDir {
public:
    explicit SkillDir(const std::string& name) : dir_(fs::temp_directory_path() / name) {
        fs::remove_all(dir_);
        fs::create_directories(dir_ / ".amber" / "skills" / "demo");
        std::ofstream skill(dir_ / ".amber" / "skills" / "demo" / "SKILL.md");
        skill << "---\nname: demo\ndescription: a demo skill\n---\nBody.\n";
    }
    ~SkillDir() { fs::remove_all(dir_); }
    std::string path() const { return dir_.string(); }

private:
    fs::path dir_;
};

// The per-agent sources, with one promoted memory and a real retriever.
struct Sources {
    std::unique_ptr<MemoryStore> store = make_memory_store(ExperienceConfig{});
    std::unique_ptr<MemoryRetriever> memory;
    ActivationSink activated;
    SessionBriefStore brief;
    PromptSources sources;

    Sources() {
        store->set_current_turn(1);
        Memory mem;
        mem.content = "the build system is make";
        mem.evidence_count = 3;
        mem.promoted = true;
        store->upsert(mem);
        memory = std::make_unique<MemoryRetriever>(*store);
        sources.memory = memory.get();
        sources.activated = &activated;
        sources.brief = &brief;
    }
};

std::unique_ptr<SkillCatalog> make_catalog(const SkillDir& dir, Config& cfg) {
    auto skills = std::make_unique<SkillCatalog>(
        cfg,
        SkillScanPaths{dir.path() + "/.amber/skills", dir.path() + "/global",
                       dir.path() + "/universal", dir.path() + "/claude", dir.path() + "/codex"},
        dir.path() + "/home");
    skills->discover({});
    return skills;
}

std::vector<std::string> texts_of(const std::vector<PromptBlock>& blocks) {
    std::vector<std::string> out;
    for (const auto& b : blocks)
        out.push_back(b.text);
    return out;
}

Message plain_message(const std::string& role, const std::string& content) {
    Message msg;
    msg.role = role;
    msg.content = content;
    return msg;
}

} // namespace

// The head and tail each carry their own blocks in priority order, and the
// source that contributed them does not change that: memory (100), discovery
// (200), plugin head (1000); activated skills (900), brief (950), plugin tail
// (1000). Insertion order breaks ties, so two plugin blocks keep registration
// order.
TEST(prompt_assembler_collects_blocks_in_priority_order) {
    SkillDir dir("amber_prompt_assembler_order");
    Workspace::set_root(dir.path());
    Config cfg;
    auto skills = make_catalog(dir, cfg);

    Sources s;
    s.sources.read_skill_available = true;
    s.sources.skills = skills.get();
    s.activated.record("demo", "the activated body");
    SessionBrief current;
    current.intent = "ship the assembler";
    s.brief.merge(current);

    PromptRegistry prompts;
    prompts.add(
        "plug", "head", 300, [] { return std::string("PLUGIN HEAD"); }, PromptPlacement::Head);
    prompts.add(
        "plug", "tail-a", 300, [] { return std::string("PLUGIN TAIL A"); }, PromptPlacement::Tail);
    prompts.add(
        "plug", "tail-b", 300, [] { return std::string("PLUGIN TAIL B"); }, PromptPlacement::Tail);
    s.sources.prompts = &prompts;

    std::vector<PromptBlock> head;
    std::vector<PromptBlock> tail;
    PromptAssembler::collect_blocks(s.sources, head, tail);

    const auto head_text = texts_of(head);
    ASSERT_EQ(head.size(), 3u);
    ASSERT(head_text[0].find("the build system is make") != std::string::npos);
    ASSERT(head_text[1].find("Available skills") != std::string::npos);
    ASSERT_EQ(head_text[2], std::string("PLUGIN HEAD"));

    const auto tail_text = texts_of(tail);
    ASSERT_EQ(tail.size(), 4u);
    ASSERT(tail_text[0].find("the activated body") != std::string::npos);
    ASSERT(tail_text[1].find("ship the assembler") != std::string::npos);
    ASSERT_EQ(tail_text[2], std::string("PLUGIN TAIL A"));
    ASSERT_EQ(tail_text[3], std::string("PLUGIN TAIL B"));
}

// With read_skill absent the discovery block would name a tool the registry
// cannot serve, so it is omitted. Activated bodies are content the model has
// already seen and stay.
TEST(prompt_assembler_omits_discovery_without_read_skill) {
    Sources s;
    s.activated.record("demo", "the activated body");

    SkillDir dir("amber_prompt_assembler_gate");
    Workspace::set_root(dir.path());
    Config cfg;
    auto skills = make_catalog(dir, cfg);
    s.sources.skills = skills.get();

    std::vector<PromptBlock> head;
    std::vector<PromptBlock> tail;
    PromptAssembler::collect_blocks(s.sources, head, tail);

    ASSERT_EQ(head.size(), 1u); // memory only
    ASSERT_EQ(tail.size(), 1u); // the activated body
    ASSERT(tail.front().text.find("the activated body") != std::string::npos);
}

// Head blocks read as instructions for this request and sit beside the system
// prompt; tail blocks follow the conversation on the user channel. The sealed
// context is never touched — this is the prompt copy.
TEST(prompt_assembler_inserts_head_after_system_and_tail_at_end) {
    Sources s;
    s.activated.record("demo", "the activated body");
    SessionBrief current;
    current.intent = "keep the tail last";
    s.brief.merge(current);

    std::vector<Message> prompt_copy;
    prompt_copy.push_back(plain_message("system", "BASE PROMPT"));
    prompt_copy.push_back(plain_message("user", "hello"));
    prompt_copy.push_back(plain_message("assistant", "hi"));

    PromptAssembler::inject_blocks(s.sources, prompt_copy);

    ASSERT_EQ(prompt_copy.front().role, std::string("system"));
    ASSERT_EQ(prompt_copy.front().content, std::string("BASE PROMPT"));
    ASSERT(prompt_copy[1].content.find("the build system is make") != std::string::npos);
    ASSERT_EQ(prompt_copy[1].role, std::string("system"));
    ASSERT_EQ(prompt_copy[2].content, std::string("hello"));
    ASSERT_EQ(prompt_copy.back().role, std::string("user"));
    ASSERT_EQ(prompt_copy.back().harness, true);
    ASSERT(prompt_copy.back().content.find("keep the tail last") != std::string::npos);
}

// The stable prefix is assembled in one documented order: base prompt,
// environment card, tool prose, System plugin blocks, mode paragraph, workspace
// note, git prose, worker boundary.
TEST(prompt_assembler_system_prompt_keeps_the_documented_order) {
    const fs::path dir = fs::temp_directory_path() / "amber_prompt_assembler_system";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "system.md");
        out << "BASE PROMPT\n";
    }
    {
        std::ofstream out(dir / "tools.md");
        out << "TOOLS PROSE\n";
    }
    {
        std::ofstream out(dir / "git.md");
        out << "GIT PROSE\n";
    }

    Config cfg;
    cfg.system_prompt_path = (dir / "system.md").string();
    cfg.tools_prompt_path = (dir / "tools.md").string();
    cfg.git_prompt_path = (dir / "git.md").string();
    cfg.mode = AgentMode::Write;

    ToolRegistry registry;
    PromptRegistry prompts;
    prompts.add(
        "plug", "system", 100, [] { return std::string("PLUGIN SYSTEM"); },
        PromptPlacement::System);

    SystemPromptSources sources;
    sources.cfg = &cfg;
    sources.registry = &registry;
    sources.prompts = &prompts;
    static const std::string environment = "ENVIRONMENT CARD";
    static const std::string workspace = "WORKSPACE NOTE";
    sources.environment_card = &environment;
    sources.workspace_note = &workspace;
    sources.subagent = true;

    const std::string system = PromptAssembler::system_prompt(sources);
    const std::vector<std::string> order = {
        "BASE PROMPT",           "ENVIRONMENT CARD", "TOOLS PROSE", "PLUGIN SYSTEM",
        "You are in WRITE mode", "WORKSPACE NOTE",   "GIT PROSE",   "## Worker directive"};
    std::size_t last = 0;
    for (const auto& needle : order) {
        const std::size_t at = system.find(needle);
        ASSERT(at != std::string::npos);
        ASSERT(at >= last);
        last = at;
    }
    fs::remove_all(dir);
}

TEST(prompt_assembler_missing_system_prompt_is_an_error) {
    Config cfg;
    cfg.system_prompt_path = "/nonexistent/amber/system.md";
    ToolRegistry registry;
    SystemPromptSources sources;
    sources.cfg = &cfg;
    sources.registry = &registry;

    bool threw = false;
    try {
        PromptAssembler::system_prompt(sources);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    ASSERT(threw);
}

TEST(prompt_assembler_system_prompt_validates_its_sources) {
    SystemPromptSources missing_all;
    bool threw = false;
    try {
        PromptAssembler::system_prompt(missing_all);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    ASSERT(threw);

    Config cfg;
    cfg.system_prompt_path = "/nonexistent/amber/system.md";
    SystemPromptSources missing_registry;
    missing_registry.cfg = &cfg;
    threw = false;
    try {
        PromptAssembler::system_prompt(missing_registry);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    ASSERT(threw);
}

TEST(prompt_assembler_missing_tools_prompt_is_an_error) {
    const fs::path dir = fs::temp_directory_path() / "amber_prompt_assembler_tools";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "system.md");
        out << "BASE PROMPT\n";
    }

    Config cfg;
    cfg.system_prompt_path = (dir / "system.md").string();
    cfg.tools_prompt_path = (dir / "missing-tools.md").string();
    ToolRegistry registry;
    SystemPromptSources sources;
    sources.cfg = &cfg;
    sources.registry = &registry;

    bool threw = false;
    try {
        PromptAssembler::system_prompt(sources);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    ASSERT(threw);
    fs::remove_all(dir);
}

// Absent sources are omitted, not guessed at: an empty registry contributes no
// tool prose, null cards contribute no section, and Read/Yolo still get their
// mode paragraph. A registry with a tool renders the advertising prose when no
// tools prompt file is configured.
TEST(prompt_assembler_system_prompt_handles_absent_sections) {
    const fs::path dir = fs::temp_directory_path() / "amber_prompt_assembler_absent";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "system.md");
        out << "BASE PROMPT\n";
    }

    Config cfg;
    cfg.system_prompt_path = (dir / "system.md").string();
    cfg.mode = AgentMode::Read;
    ToolRegistry empty_registry;
    SystemPromptSources sources;
    sources.cfg = &cfg;
    sources.registry = &empty_registry;
    static const std::string empty_text;
    sources.environment_card = &empty_text;
    sources.workspace_note = &empty_text;

    std::string system = PromptAssembler::system_prompt(sources);
    ASSERT(system.find("You are in READ mode") != std::string::npos);
    ASSERT(system.find("## Plugins") == std::string::npos);

    cfg.mode = AgentMode::Yolo;
    ASSERT(PromptAssembler::system_prompt(sources).find("You are in YOLO mode") !=
           std::string::npos);

    cfg.mode = AgentMode::Write;
    ToolRegistry with_tool;
    with_tool.register_tool(std::make_unique<NamedTool>("demo_tool"));
    sources.registry = &with_tool;
    system = PromptAssembler::system_prompt(sources);
    ASSERT(system.find("demo_tool") != std::string::npos);

    // A mode outside the documented set adds no paragraph: the fallback is a
    // prompt without a claim about how the agent is operating.
    cfg.mode = static_cast<AgentMode>(99);
    system = PromptAssembler::system_prompt(sources);
    ASSERT(system.find("You are in ") == std::string::npos);
    fs::remove_all(dir);
}

// A blank section file is a section that renders nothing: it neither adds a
// separator nor makes the prompt drift. The same rule applies to an empty
// environment card and workspace note.
TEST(prompt_assembler_skips_blank_sections) {
    const fs::path dir = fs::temp_directory_path() / "amber_prompt_assembler_blank";
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "system.md");
        out << "BASE PROMPT\n";
    }
    {
        std::ofstream out(dir / "tools.md");
        out << "\n\n";
    }

    Config cfg;
    cfg.system_prompt_path = (dir / "system.md").string();
    cfg.tools_prompt_path = (dir / "tools.md").string();
    ToolRegistry registry;
    SystemPromptSources sources;
    sources.cfg = &cfg;
    sources.registry = &registry;

    const std::string system = PromptAssembler::system_prompt(sources);
    ASSERT_EQ(system.rfind("BASE PROMPT", 0), (std::size_t)0);
    ASSERT(system.find("You are in WRITE mode") != std::string::npos);
    fs::remove_all(dir);
}

TEST(prompt_assembler_order_blocks_breaks_ties_by_insertion) {
    std::vector<PromptBlock> blocks = {
        {500, 2, "second"}, {100, 0, "first"}, {500, 1, "tie winner"}};
    PromptAssembler::order_blocks(blocks);
    ASSERT_EQ(blocks[0].text, std::string("first"));
    ASSERT_EQ(blocks[1].text, std::string("tie winner"));
    ASSERT_EQ(blocks[2].text, std::string("second"));
}

TEST(prompt_assembler_empty_sources_add_no_blocks) {
    PromptSources sources;
    std::vector<PromptBlock> head;
    std::vector<PromptBlock> tail;
    PromptAssembler::collect_blocks(sources, head, tail);
    ASSERT(head.empty());
    ASSERT(tail.empty());

    std::vector<Message> prompt_copy;
    prompt_copy.push_back(plain_message("system", "BASE PROMPT"));
    PromptAssembler::inject_blocks(sources, prompt_copy);
    ASSERT_EQ(prompt_copy.size(), 1u);
}

// A retriever with nothing to say and an empty brief are both silent: the
// block is omitted rather than pushed as an empty message.
TEST(prompt_assembler_omits_empty_memory_and_brief) {
    auto store = make_memory_store(ExperienceConfig{});
    MemoryRetriever memory(*store);
    SessionBriefStore brief;
    PromptSources sources;
    sources.memory = &memory;
    sources.brief = &brief;

    std::vector<PromptBlock> head;
    std::vector<PromptBlock> tail;
    PromptAssembler::collect_blocks(sources, head, tail);
    ASSERT(head.empty());
    ASSERT(tail.empty());
}

TEST(prompt_assembler_omits_empty_discovery) {
    const fs::path dir = fs::temp_directory_path() / "amber_prompt_assembler_empty_discovery";
    fs::remove_all(dir);
    fs::create_directories(dir);
    Config cfg;
    const std::string none = (dir / "none").string();
    SkillCatalog empty(cfg, SkillScanPaths{none, none, none, none, none}, (dir / "home").string());
    empty.discover({});
    PromptSources sources;
    sources.read_skill_available = true;
    sources.skills = &empty;

    std::vector<PromptBlock> head;
    std::vector<PromptBlock> tail;
    PromptAssembler::collect_blocks(sources, head, tail);
    ASSERT(head.empty());
    ASSERT(tail.empty());
    fs::remove_all(dir);
}

// A catalog can contribute discovery while the activation sink is absent; the
// missing sink is a missing source, not an empty list that has to exist.
TEST(prompt_assembler_supports_skills_without_an_activation_sink) {
    SkillDir dir("amber_prompt_assembler_no_sink");
    Workspace::set_root(dir.path());
    Config cfg;
    auto skills = make_catalog(dir, cfg);
    PromptSources sources;
    sources.read_skill_available = true;
    sources.skills = skills.get();
    sources.activated = nullptr;

    std::vector<PromptBlock> head;
    std::vector<PromptBlock> tail;
    PromptAssembler::collect_blocks(sources, head, tail);
    ASSERT_EQ(head.size(), 1u);
    ASSERT(head.front().text.find("Available skills") != std::string::npos);
    ASSERT(tail.empty());
}

// A prompt copy with no system message (a host replaying a raw conversation)
// still receives its head blocks, at the front.
TEST(prompt_assembler_injects_without_a_system_message) {
    Sources s;
    std::vector<Message> prompt_copy;
    prompt_copy.push_back(plain_message("user", "hello"));
    prompt_copy.push_back(plain_message("assistant", "hi"));

    PromptAssembler::inject_blocks(s.sources, prompt_copy);
    ASSERT_EQ(prompt_copy.front().role, std::string("system"));
    ASSERT(prompt_copy.front().content.find("the build system is make") != std::string::npos);
    ASSERT_EQ(prompt_copy[1].content, std::string("hello"));
}
