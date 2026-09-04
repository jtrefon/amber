
#include <csignal>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "agent/mcp_commands.h"
#include "agent/mcp_config.h"
#include "agent/prompt.h"
#include "agent/registry.h"
#include "agent/workspace.h"
#include "tests/test_util.h"

namespace {

std::string run_cmd(const std::string& cmd) {
    FILE* pipe = popen(cmd.c_str(), "r");
    std::string out;
    if (!pipe) return out;
    char buf[256];
    while (fgets(buf, sizeof buf, pipe)) out += buf;
    pclose(pipe);
    return out;
}

void write_file(const std::string& path, const std::string& contents) {
    run_cmd("mkdir -p " + std::string(path).substr(0, path.find_last_of('/')));
    std::ofstream f(path);
    f << contents;
}

struct McpEnv {
    std::string ws;
    std::string home;
    std::string project_mcp;
    std::string global_mcp;
    std::string cwd;

    explicit McpEnv(const std::string& tag)
        : ws("/tmp/amber_mcp_" + tag),
          home("/tmp/amber_mcp_home_" + tag) {
        project_mcp = ws + "/.amber/mcp";
        global_mcp = home + "/.config/amber/mcp";
        run_cmd("rm -rf " + ws + " " + home);
        agent::Workspace::set_root(ws);
        setenv("HOME", home.c_str(), 1);
        unsetenv("XDG_CONFIG_HOME");
        unsetenv("AMBER_MCP_SERVERS");
        char buf[4096];
        cwd = getcwd(buf, sizeof buf) ? buf : ".";
    }
};

} // namespace

// Global + project configs merge, project wins on name collisions.
TEST(mcp_config_precedence_project_wins) {
    McpEnv env("prec");
    write_file(env.global_mcp + "/github.conf",
               "type=stdio\ncommand=/usr/bin/github-server\nauto_connect=1\n");
    write_file(env.project_mcp + "/github.conf",
               "type=stdio\ncommand=/usr/bin/project-github\ntrusted=1\n");
    write_file(env.global_mcp + "/global-only.conf",
               "type=http\nurl=https://example.com/mcp\n");

    auto servers = agent::load_mcp_servers();
    ASSERT_EQ(servers.size(), 2u);
    ASSERT(servers.count("github") == 1u);
    ASSERT_EQ(servers["github"].command, "/usr/bin/project-github");
    ASSERT_TRUE(servers["github"].trusted);
    ASSERT_TRUE(servers["github"].auto_connect);
    ASSERT(servers.count("global-only") == 1u);
    ASSERT_EQ(servers["global-only"].url, "https://example.com/mcp");
}

// Invalid configs are listed with an error, never fatal.
TEST(mcp_config_validation_errors) {
    McpEnv env("val");
    write_file(env.project_mcp + "/bad-type.conf", "command=/x\n");
    write_file(env.project_mcp + "/no-command.conf", "type=stdio\n");
    write_file(env.project_mcp + "/no-url.conf", "type=http\n");
    write_file(env.project_mcp + "/ok.conf",
               "type=stdio\ncommand=/usr/bin/ok\n");

    auto servers = agent::load_mcp_servers();
    ASSERT_EQ(servers.size(), 4u);
    ASSERT_FALSE(servers["bad-type"].error.empty());
    ASSERT_FALSE(servers["no-command"].error.empty());
    ASSERT_FALSE(servers["no-url"].error.empty());
    ASSERT_TRUE(servers["ok"].error.empty());
}

// args are space-separated; timeout and flags parse.
TEST(mcp_config_field_parsing) {
    McpEnv env("fields");
    write_file(env.project_mcp + "/srv.conf",
               "type=stdio\ncommand=/usr/bin/srv\n"
               "args=--port 8080 --verbose\ncwd=/tmp\n"
               "enabled=0\nauto_connect=1\ntrusted=1\ntimeout_s=42\n");

    auto servers = agent::load_mcp_servers();
    ASSERT_EQ(servers["srv"].args.size(), 3u);
    ASSERT_EQ(servers["srv"].args[0], "--port");
    ASSERT_EQ(servers["srv"].args[2], "--verbose");
    ASSERT_EQ(servers["srv"].cwd, "/tmp");
    ASSERT_FALSE(servers["srv"].enabled);
    ASSERT_TRUE(servers["srv"].auto_connect);
    ASSERT_TRUE(servers["srv"].trusted);
    ASSERT_EQ(servers["srv"].timeout_s, 42);
}

// AMBER_MCP_SERVERS overrides `enabled` for a run.
TEST(mcp_config_env_override) {
    McpEnv env("env");
    write_file(env.project_mcp + "/a.conf", "type=stdio\ncommand=/x\nenabled=0\n");
    write_file(env.project_mcp + "/b.conf", "type=stdio\ncommand=/x\nenabled=1\n");
    setenv("AMBER_MCP_SERVERS", "a", 1);

    auto servers = agent::load_mcp_servers();
    ASSERT_TRUE(servers["a"].enabled);
    ASSERT_FALSE(servers["b"].enabled);
}

// save/delete round-trip into the project config dir.
TEST(mcp_config_save_delete_roundtrip) {
    McpEnv env("save");
    agent::McpServerConfig cfg;
    cfg.name = "saved";
    cfg.type = "http";
    cfg.url = "https://host/mcp";
    cfg.auth_token = "tok";
    cfg.trusted = true;
    ASSERT_TRUE(agent::save_mcp_server(cfg));

    auto servers = agent::load_mcp_servers();
    ASSERT_EQ(servers["saved"].url, "https://host/mcp");
    ASSERT_EQ(servers["saved"].auth_token, "tok");
    ASSERT_TRUE(servers["saved"].trusted);

    ASSERT_TRUE(agent::delete_mcp_server("saved"));
    ASSERT_EQ(agent::load_mcp_servers().count("saved"), 0u);
}

// connect_all respects enabled + auto_connect; a real stdio server connects.
TEST(mcp_manager_connect_all) {
    McpEnv env("conn");
    write_file(env.project_mcp + "/echo.conf",
               "type=stdio\ncommand=python3\n"
               "args=tests/fixtures/mcp_echo.py\ncwd=" + env.cwd + "\nauto_connect=1\n");
    write_file(env.project_mcp + "/off.conf",
               "type=stdio\ncommand=python3\n"
               "args=tests/fixtures/mcp_echo.py\ncwd=" + env.cwd + "\nenabled=0\n");

    agent::ServerManager mgr(agent::load_mcp_servers());
    mgr.connect_all();
    auto snap = mgr.snapshot();
    ASSERT_EQ(snap.size(), 2u);
    for (const auto& s : snap) {
        if (s.name == "echo") {
            ASSERT(s.connected);
            ASSERT_EQ(s.error, "");
        }
        if (s.name == "off") {
            ASSERT_FALSE(s.connected);
        }
    }
    mgr.shutdown_all();
}

// Explicit connect works; connecting a disabled server is refused.
TEST(mcp_manager_connect_disabled_refused) {
    McpEnv env("dis");
    write_file(env.project_mcp + "/off.conf",
               "type=stdio\ncommand=python3\n"
               "args=tests/fixtures/mcp_echo.py\ncwd=" + env.cwd + "\nenabled=0\n");

    agent::ServerManager mgr(agent::load_mcp_servers());
    std::string err = mgr.connect("off");
    ASSERT_FALSE(err.empty());
    ASSERT(mgr.client("off") == nullptr);
}

// A spawn failure lands in the snapshot error, not a crash.
TEST(mcp_manager_spawn_failure_reported) {
    McpEnv env("spawn");
    write_file(env.project_mcp + "/bad.conf",
               "type=stdio\ncommand=no-such-binary\n");

    agent::ServerManager mgr(agent::load_mcp_servers());
    std::string err = mgr.connect("bad");
    ASSERT_FALSE(err.empty());
    auto snap = mgr.snapshot();
    ASSERT_FALSE(snap[0].connected);
    ASSERT_FALSE(snap[0].error.empty());
}

// trust/enabled toggles persist to the project config immediately.
TEST(mcp_manager_trust_toggle_persists) {
    McpEnv env("trust");
    write_file(env.project_mcp + "/srv.conf", "type=stdio\ncommand=/x\n");

    agent::ServerManager mgr(agent::load_mcp_servers());
    ASSERT_FALSE(mgr.trusted("srv"));
    ASSERT_EQ(mgr.set_trusted("srv", true), "");
    ASSERT_TRUE(mgr.trusted("srv"));
    ASSERT_TRUE(agent::load_mcp_servers()["srv"].trusted);
    ASSERT_EQ(mgr.set_enabled("srv", false), "");
    ASSERT_FALSE(mgr.enabled("srv"));
    ASSERT_FALSE(agent::load_mcp_servers()["srv"].enabled);
}

// A real HTTP server connects through the manager.
TEST(mcp_manager_http_connect) {
    McpEnv env("http");
    std::string statefile = "/tmp/mcp_mgr_http.txt";
    unlink(statefile.c_str());
    std::string cmd = "python3 tests/fixtures/mcp_http_server.py " +
                      statefile + " echo >/dev/null 2>&1 &";
    ASSERT(std::system(cmd.c_str()) == 0);
    std::ifstream f;
    int port = -1;
    int pid = -1;
    for (int i = 0; i < 300 && port < 0; ++i) {  // ~3s: Python fixture boot on slow CI
        usleep(10 * 1000);
        f.open(statefile);
        if (f.is_open()) {
            std::string line;
            while (std::getline(f, line)) {
                if (line.rfind("PORT:", 0) == 0)
                    port = std::stoi(line.substr(5));
                if (line.rfind("PID:", 0) == 0) pid = std::stoi(line.substr(4));
            }
            f.close();
        }
    }
    ASSERT(port > 0);
    write_file(env.project_mcp + "/remote.conf",
               "type=http\nurl=http://127.0.0.1:" + std::to_string(port) +
                   "/mcp\n");

    agent::ServerManager mgr(agent::load_mcp_servers());
    ASSERT_EQ(mgr.connect("remote"), "");
    ASSERT(mgr.client("remote") != nullptr);
    ASSERT(mgr.client("remote")->connected());
    mgr.shutdown_all();
    if (pid > 0) {
        kill(pid, SIGKILL);
        int st = 0;
        waitpid(pid, &st, 0);
    }
    unlink(statefile.c_str());
}

// [MS-08] Token-bearing configs are written 0600 and never surface in the
// manager snapshot.
TEST(mcp_config_token_redaction) {
    McpEnv env("redact");
    agent::McpServerConfig cfg;
    cfg.name = "db";
    cfg.type = "http";
    cfg.url = "https://db/mcp";
    cfg.auth_token = "super-secret-token";
    ASSERT_TRUE(agent::save_mcp_server(cfg));

    struct stat st;
    ASSERT_EQ(stat((env.project_mcp + "/db.conf").c_str(), &st), 0);
    ASSERT_EQ(static_cast<int>(st.st_mode) & 0777, 0600);

    agent::ServerManager mgr(agent::load_mcp_servers());
    std::string snap_text;
    for (const auto& s : mgr.snapshot()) snap_text += s.name + s.type;
    ASSERT(snap_text.find("super-secret-token") == std::string::npos);
}

// ---------------------------------------------------------------------------
// Command backends ([MU-01] list, [MU-03] prompts, prompt get/set)
// ---------------------------------------------------------------------------

namespace {

struct EchoManager {
    std::string cwd;
    std::string ws;
    agent::ServerManager mgr;
    agent::ToolRegistry reg;

    EchoManager()
        : mgr({{"echo", [&]() {
                   char buf[4096];
                   cwd = getcwd(buf, sizeof buf) ? buf : ".";
                   ws = "/tmp/amber_mcp_cmd_ws";
                   run_cmd("rm -rf " + ws);
                   agent::Workspace::set_root(ws);
                   agent::McpServerConfig cfg;
                   cfg.name = "echo";
                   cfg.type = "stdio";
                   cfg.command = "python3";
                   cfg.args = {"tests/fixtures/mcp_echo.py"};
                   cfg.cwd = cwd;
                   return cfg;
               }()}}) {}
};

} // namespace

// [MU-01] /mcp list renders state lines.
TEST(mcp_commands_list_lines) {
    EchoManager env;
    ASSERT_EQ(agent::mcp_connect(env.mgr, env.reg, "echo"), "");
    auto lines = agent::mcp_list_lines(env.mgr);
    ASSERT_EQ(lines.size(), 1u);
    ASSERT(lines[0].find("echo") != std::string::npos);
    ASSERT(lines[0].find("connected") != std::string::npos);
    ASSERT(lines[0].find("1 tools") != std::string::npos);
    agent::mcp_disconnect(env.mgr, env.reg, "echo");
    auto lines2 = agent::mcp_list_lines(env.mgr);
    ASSERT(lines2[0].find("disconnected") != std::string::npos);
    env.mgr.shutdown_all();
}

// [MU-02] connect registers adapters; disconnect removes them.
TEST(mcp_commands_connect_registers_tools) {
    EchoManager env;
    ASSERT_EQ(agent::mcp_connect(env.mgr, env.reg, "echo"), "");
    ASSERT(env.reg.find("mcp_echo_echo_tool") != nullptr);
    agent::mcp_disconnect(env.mgr, env.reg, "echo");
    ASSERT(env.reg.find("mcp_echo_echo_tool") == nullptr);
    env.mgr.shutdown_all();
}

// [MU-03] /prompt get validates args and returns the flattened template.
TEST(mcp_commands_prompt_get) {
    EchoManager env;
    ASSERT_EQ(agent::mcp_connect(env.mgr, env.reg, "echo"), "");
    std::string text;
    std::string err = agent::mcp_prompt(env.mgr, "echo", "greet",
                                        {{"name", "bob"}}, text);
    ASSERT_EQ(err, "");
    ASSERT(text.find("user: greet bob") != std::string::npos);

    std::string text2;
    std::string err2 = agent::mcp_prompt(env.mgr, "echo", "greet",
                                         json::object(), text2);
    ASSERT_FALSE(err2.empty());
    ASSERT(err2.find("missing required argument 'name'") !=
           std::string::npos);

    std::string text3;
    std::string err3 = agent::mcp_prompt(env.mgr, "echo", "ghost",
                                         json::object(), text3);
    ASSERT_FALSE(err3.empty());
    ASSERT(err3.find("unknown prompt") != std::string::npos);
    env.mgr.shutdown_all();
}

// refresh re-registers adapters after a discovery change.
TEST(mcp_commands_refresh) {
    EchoManager env;
    ASSERT_EQ(agent::mcp_connect(env.mgr, env.reg, "echo"), "");
    ASSERT_EQ(agent::mcp_refresh(env.mgr, env.reg, "echo"), "");
    ASSERT(env.reg.find("mcp_echo_echo_tool") != nullptr);
    env.mgr.shutdown_all();
}

// prompts/mcp.md loads non-empty at session start.
TEST(mcp_prompts_file_loaded) {
    std::string p = agent::load_prompt("prompts/mcp.md");
    ASSERT_FALSE(p.empty());
    ASSERT(p.find("mcp_") != std::string::npos);
    ASSERT(p.find("the user invokes") != std::string::npos);
}
