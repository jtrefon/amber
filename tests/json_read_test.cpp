
// Scalar reads on JSON amber does not build. json::value(key, default) falls
// back only when the key is ABSENT — a key present with a different type throws
// type_error.302 instead of coercing. Every payload here crosses a trust
// boundary: model tool arguments, an MCP server's schema and replies, an
// external plugin's replies, a tool's own metadata, or a store file on disk.
// A drifted value is adopted whenever it can be read for what it means (1
// stands for true, "true" for true); a genuinely uninterpretable one costs only
// that field, never the caller.

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include "agent/agent_helpers.h"
#include "agent/experience.h"
#include "agent/job.h"
#include "agent/mcp_client.h"
#include "agent/plugin.h"
#include "agent/search_backend.h"
#include "agent/tools.h"
#include "agent/workspace.h"
#include "mcp_test_util.h"
#include "tests/test_util.h"

namespace {

std::string cwd() {
    char buf[4096];
    return getcwd(buf, sizeof buf) ? buf : ".";
}

// A fresh directory under /tmp, so nothing here reads or writes the
// developer's real XDG roots.
std::string scratch_dir(const std::string& name) {
    const std::string dir = "/tmp/amber_json_read/" + name;
    const std::string cmd = "rm -rf " + dir + " && mkdir -p " + dir;
    if (std::system(cmd.c_str()) != 0)
        return "";
    return dir;
}

bool write_file(const std::string& path, const std::string& body, mode_t mode = 0644) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    if (ec)
        return false;
    std::ofstream f(path, std::ios::binary);
    if (!f)
        return false;
    f << body;
    f.close();
    return chmod(path.c_str(), mode) == 0;
}

// XDG_CONFIG_HOME restored on scope exit: the plugin manager resolves its state
// file through it, and a leaked value would follow every later test.
struct XdgConfigGuard {
    explicit XdgConfigGuard(const std::string& value) {
        if (const char* old = std::getenv("XDG_CONFIG_HOME"))
            saved_ = old;
        setenv("XDG_CONFIG_HOME", value.c_str(), 1);
    }
    ~XdgConfigGuard() {
        if (saved_.empty())
            unsetenv("XDG_CONFIG_HOME");
        else
            setenv("XDG_CONFIG_HOME", saved_.c_str(), 1);
    }
    XdgConfigGuard(const XdgConfigGuard&) = delete;
    XdgConfigGuard& operator=(const XdgConfigGuard&) = delete;

private:
    std::string saved_;
};

// A plugin install: a manifest that validates, plus an executable `main` that
// answers the newline-delimited JSON-RPC handshake and then one reply per call.
// It keeps reading until stdin closes, like a real plugin: a fixture that exits
// early would make the caller's second write race the child's death.
bool write_plugin(const std::string& root, const std::string& id, const std::string& handshake,
                  const std::string& call_reply) {
    const std::string dir = root + "/" + id;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec)
        return false;
    if (!write_file(dir + "/manifest.json",
                    "{\"id\":\"" + id +
                        "\",\"name\":\"Fixture\",\"version\":\"1.0.0\",\"protocol_version\":1,"
                        "\"main\":\"plugin-bin\",\"description\":\"json_read fixture\","
                        "\"tools\":[{\"name\":\"ping\",\"description\":\"Ping the fixture.\","
                        "\"schema\":{\"type\":\"object\",\"properties\":{}}}]}"))
        return false;
    const std::string body = "#!/bin/sh\nread -r _line && printf '%s\\n' '" + handshake +
                             "'\nwhile read -r _line; do printf '%s\\n' '" + call_reply +
                             "'; done\n";
    return write_file(dir + "/plugin-bin", body, 0755);
}

} // namespace

// ---------------------------------------------------------------------------
// The fatal one: a tool result's own metadata, read once per tool call.
// ---------------------------------------------------------------------------

// bash_tool records that it timed out in meta["timeout"]; format_tool_envelope
// reads the same key as a flag. A command that was cut short therefore killed
// the whole turn with type_error.302 before this read was type-guarded. A number
// is adopted for what it means: seconds waited is still a timeout.
TEST(json_read_envelope_adopts_a_numeric_timeout_flag) {
    agent::ToolResult timed_out;
    timed_out.ok = false;
    timed_out.error = "timed out after 120s";
    timed_out.meta = {{"exit", 0}, {"duration_ms", 120000L}, {"timeout", 120}};

    const std::string env =
        agent::format_tool_envelope("bash", {{"command", "make test"}}, timed_out);
    ASSERT(env.find("status=timeout") != std::string::npos);
    ASSERT(env.find("type_error") == std::string::npos);
}

TEST(json_read_envelope_survives_null_flags) {
    agent::ToolResult r;
    r.ok = false;
    r.error = "no such file";
    r.meta = {{"denied", nullptr}, {"timeout", nullptr}};

    const std::string env = agent::format_tool_envelope("read", {{"path", "a.txt"}}, r);
    ASSERT(env.find("status=error") != std::string::npos);
}

// The spellings a drifted payload uses for a true flag are adopted; one that
// carries no meaning ("no" is not a boolean spelling) falls back without
// costing the caller.
TEST(json_read_envelope_adopts_a_string_flag) {
    agent::ToolResult r;
    r.ok = false;
    r.error = "denied";
    r.meta = {{"denied", "true"}, {"timeout", "no"}};

    const std::string env = agent::format_tool_envelope("bash", json::object(), r);
    ASSERT(env.find("status=denied") != std::string::npos);
    ASSERT(env.find("type_error") == std::string::npos);
}

// The shape the readers were written for keeps working: a real flag still names
// the status, so guarding the read did not flatten the field.
TEST(json_read_envelope_names_a_real_flag) {
    agent::ToolResult denied;
    denied.ok = false;
    denied.error = "declined";
    denied.meta = {{"denied", true}, {"timeout", true}};
    ASSERT(agent::format_tool_envelope("bash", json::object(), denied).find("status=denied") !=
           std::string::npos);

    agent::ToolResult timed_out;
    timed_out.ok = false;
    timed_out.error = "timed out";
    timed_out.meta = {{"timeout", true}};
    ASSERT(agent::format_tool_envelope("bash", json::object(), timed_out).find("status=timeout") !=
           std::string::npos);
}

// The producing end: a cut-short command must record the flag its readers
// assume, not the seconds it waited. bench_test's recorder reads the same key.
TEST(json_read_bash_timeout_records_a_flag_not_the_seconds) {
    auto tool = agent::make_bash_tool();
    auto r = tool->execute({{"command", "sleep 5"}, {"timeout", 1}});
    ASSERT_FALSE(r.ok);
    ASSERT(r.error.find("timed out") != std::string::npos);
    ASSERT(r.meta.contains("timeout"));
    ASSERT(r.meta["timeout"].is_boolean());
    ASSERT(r.meta["timeout"].get<bool>());
    // The seconds still reach the model, in the error and in the output.
    ASSERT(r.error.find("1s") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Model tool arguments.
// ---------------------------------------------------------------------------

TEST(json_read_process_read_adopts_a_numeric_all_flag) {
    agent::JobService jobs;
    auto tools = agent::make_process_tools(jobs);
    const agent::Tool* start = nullptr;
    const agent::Tool* read = nullptr;
    for (const auto& t : tools) {
        if (t->name() == "process_start")
            start = t.get();
        if (t->name() == "process_read")
            read = t.get();
    }
    ASSERT(start != nullptr);
    ASSERT(read != nullptr);

    const auto started =
        start->execute({{"command", "printf 'one\\ntwo\\n'"}, {"idle_timeout", 5}});
    ASSERT(started.ok);

    // 1 stands for true: the whole output, not the delta. The job's output is
    // read asynchronously, so poll rather than assume it has landed.
    agent::ToolResult r;
    for (int i = 0; i < 100; ++i) {
        r = read->execute({{"id", started.output}, {"all", 1}});
        if (r.output.find("one") != std::string::npos)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT(r.ok);
    ASSERT(r.output.find("one") != std::string::npos);
    ASSERT(r.output.find("two") != std::string::npos);

    // The real flag still selects the full output.
    const auto full = read->execute({{"id", started.output}, {"all", true}});
    ASSERT(full.ok);
}

// A model that spells its limits as text ("3", "1") used to lose the call: the
// read threw type_error.302 before the tool saw the argument.
TEST(json_read_read_tool_adopts_a_string_line_range) {
    agent::Workspace::set_root(cwd());
    auto tool = agent::make_read_tool();
    const auto r = tool->execute({{"path", "Makefile"}, {"offset", "1"}, {"limit", "3"}});
    ASSERT(r.ok);
    // The page is the requested one, not the 200-line default.
    ASSERT(r.output.find(":1-3 (3 lines)") != std::string::npos);
}

TEST(json_read_search_tool_adopts_a_string_max) {
    const std::string dir = scratch_dir("search_max");
    ASSERT(!dir.empty());
    ASSERT(write_file(dir + "/hits.txt", "hit\nhit\nhit\nhit\n"));
    agent::Workspace::set_root(dir);
    auto tool = agent::make_search_tool(agent::builtin_search_backend_provider());

    const auto three = tool->execute({{"pattern", "hit"}, {"max", "3"}});
    const auto one = tool->execute({{"pattern", "hit"}, {"max", 1}});
    ASSERT(three.ok);
    ASSERT(one.ok);
    ASSERT(three.output.find("hits.txt") != std::string::npos);
    // The string spelling was adopted: three matches really are more than one.
    ASSERT(three.output.size() > one.output.size());
    agent::Workspace::set_root(cwd());
}

TEST(json_read_bash_tool_adopts_a_string_timeout) {
    auto tool = agent::make_bash_tool();
    const auto r = tool->execute({{"command", "sleep 5"}, {"timeout", "1"}});
    ASSERT_FALSE(r.ok);
    ASSERT(r.error.find("timed out") != std::string::npos);
    ASSERT(r.meta["timeout"].is_boolean());
}

// ---------------------------------------------------------------------------
// An MCP server's schema: third-party input.
// ---------------------------------------------------------------------------

TEST(json_read_mcp_prompt_schema_adopts_a_numeric_required_field) {
    auto transport = std::make_unique<mcp_test::FakeTransport>();
    mcp_test::FakeTransport* raw = transport.get();
    auto client = std::make_unique<agent::MCPClient>("fixture", std::move(transport));

    const json scope_arg = {{"name", "scope"}, {"required", 1}};
    const json depth_arg = {{"name", "depth"}, {"required", true}};
    const json prompt = {{"name", "review"}, {"arguments", json::array({scope_arg, depth_arg})}};

    raw->script.push_back(mcp_test::ok_result(mcp_test::init_result()));
    raw->script.push_back(mcp_test::ok_result({{"tools", json::array()}}));
    raw->script.push_back(mcp_test::ok_result({{"resources", json::array()}}));
    raw->script.push_back(mcp_test::ok_result({{"prompts", json::array({prompt})}}));

    ASSERT_EQ(client->connect(), std::string());
    ASSERT_EQ(client->prompts().size(), std::size_t{1});
    // 1 stands for true in this schema too: both arguments are required.
    ASSERT_EQ(client->prompts()[0].required_args.size(), std::size_t{2});
}

TEST(json_read_mcp_call_tool_adopts_a_numeric_is_error) {
    auto transport = std::make_unique<mcp_test::FakeTransport>();
    mcp_test::FakeTransport* raw = transport.get();
    auto client = std::make_unique<agent::MCPClient>("fixture", std::move(transport));
    mcp_test::script_connect(*raw);
    ASSERT_EQ(client->connect(), std::string());

    // 1 stands for true: a numeric isError is still a failed call.
    raw->script.push_back(mcp_test::ok_result(
        {{"isError", 1}, {"content", json::array({{{"type", "text"}, {"text", "boom"}}})}}));
    const auto r = client->call_tool("get_issue", json::object());
    ASSERT_FALSE(r.ok);
    ASSERT_EQ(r.error, std::string("boom"));
}

TEST(json_read_mcp_init_adopts_a_numeric_protocol_version) {
    auto transport = std::make_unique<mcp_test::FakeTransport>();
    mcp_test::FakeTransport* raw = transport.get();
    auto client = std::make_unique<agent::MCPClient>("fixture", std::move(transport));
    raw->script.push_back(mcp_test::ok_result({{"protocolVersion", 1}}));

    // The version is read as text, so a numeric one is reported (and refused)
    // instead of throwing out of connect().
    ASSERT_EQ(client->connect(), std::string("unsupported protocol version: 1"));
    ASSERT_FALSE(client->connected());
}

// ---------------------------------------------------------------------------
// External plugin replies.
// ---------------------------------------------------------------------------

TEST(json_read_plugin_discover_survives_a_numeric_enabled_field) {
    const std::string cfg = scratch_dir("plugin_cfg");
    const std::string root = scratch_dir("plugin_root");
    ASSERT(!cfg.empty());
    ASSERT(!root.empty());
    ASSERT(write_plugin(root, "alpha", R"({"id":1,"result":{"ok":true}})", "{}"));
    ASSERT(write_file(cfg + "/amber/plugins/state.json",
                      "{\"alpha\":{\"enabled\":1},\"beta\":{\"enabled\":true}}"));

    XdgConfigGuard guard(cfg);
    agent::PluginManager manager;
    manager.discover({root});

    ASSERT_EQ(manager.plugins().size(), std::size_t{1});
    // 1 stands for true, so the plugin reports itself enabled.
    ASSERT(manager.plugins()[0].state == agent::PluginState::Enabled);
}

TEST(json_read_plugin_reply_adopts_scalar_type_drift) {
    const std::string cfg = scratch_dir("plugin_cfg_reply");
    const std::string root = scratch_dir("plugin_root_reply");
    ASSERT(!cfg.empty());
    ASSERT(!root.empty());
    // A well-formed handshake, then a tool reply whose fields drifted: `ok` is a
    // number and `output` is one too.
    ASSERT(write_plugin(root, "alpha", R"({"id":1,"result":{"ok":true}})",
                        R"({"id":2,"result":{"ok":1,"output":7}})"));
    ASSERT(write_file(cfg + "/amber/plugins/state.json", "{}"));

    XdgConfigGuard guard(cfg);
    agent::PluginManager manager;
    manager.discover({root});
    agent::ToolRegistry registry;
    ASSERT(manager.enable("alpha", registry));

    const auto tool = registry.find("plugin_alpha_ping");
    ASSERT(tool != nullptr);
    const agent::ToolResult r = tool->execute(json::object());
    // The reply decoded instead of throwing: a numeric `ok` is adopted as the
    // boolean it means, and a numeric output renders as the text it is.
    ASSERT(r.ok);
    ASSERT_EQ(r.output, std::string("7"));
}

// ---------------------------------------------------------------------------
// Amber's own persisted experience store.
// ---------------------------------------------------------------------------

TEST(json_read_memory_store_loads_past_a_drifted_field) {
    const std::string dir = scratch_dir("store");
    ASSERT(!dir.empty());
    const std::string path = dir + "/store.json";
    ASSERT(write_file(path, "{\"version\":1,"
                            "\"memories\":[{\"id\":\"m1\",\"name\":\"idiom\","
                            "\"content\":\"box the loop\",\"promoted\":1}],"
                            "\"skills\":[{\"id\":\"s1\",\"name\":\"sk\",\"content\":\"c\","
                            "\"promoted\":\"yes\"}]}"));

    agent::ExperienceConfig config;
    auto store = agent::make_memory_store(config);
    // One drifted field used to fail the whole load; now it costs only itself.
    ASSERT(store->load(path));
    ASSERT_EQ(store->store_size(), std::size_t{2});
}
