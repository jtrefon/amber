// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent.h"
#include "agent/tools.h"
#include "agent/search_backend.h"
#include "tui/textutil.h"
#include "tui/palette.h"
#include "tests/test_util.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

static inline void run_cmd(const std::string& cmd) {
    int rc = std::system(cmd.c_str());
    (void)rc;
}

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

TEST(config_defaults) {
    agent::Config c;
    ASSERT_EQ(c.api_base, "http://localhost:8000/v1");
    ASSERT_EQ(c.model, "gpt-4o-mini");
    ASSERT_EQ(c.max_tool_iterations, 32);
    ASSERT_TRUE(c.stream);
    ASSERT_EQ(c.api_url(), "http://localhost:8000/v1/chat/completions");
}

TEST(config_validate_accepts_defaults) {
    agent::Config c;
    ASSERT_TRUE(c.validate().empty());
}

TEST(config_validate_flags_problems) {
    agent::Config c;
    c.api_base = "localhost:8000/v1";   // missing scheme
    c.model = "";                       // empty
    c.max_tool_iterations = 0;          // too small
    c.temperature = 5.0;                // out of range
    c.max_tokens = 0;                   // zero
    c.thinking = "sometimes";           // invalid enum
    auto errs = c.validate();
    ASSERT(errs.size() >= 6);

    agent::Config trailing;
    trailing.api_base = "http://localhost:8000/v1/";  // trailing slash
    ASSERT_FALSE(trailing.validate().empty());
}

// ---------------------------------------------------------------------------
// TUI text utilities (UTF-8 wrapping / width / decoding)
// ---------------------------------------------------------------------------

TEST(textutil_utf8_len_and_display_cols) {
    std::string ascii = "hello";
    ASSERT_EQ(tui::text::utf8_len(ascii, 0), (size_t)1);
    ASSERT_EQ(tui::text::display_cols(ascii), 5);

    std::string emoji = "a\xF0\x9F\x98\x80z";  // a + U+1F600 + z
    ASSERT_EQ(tui::text::utf8_len(emoji, 1), (size_t)4);
    ASSERT_EQ(tui::text::display_cols(emoji), 3);

    // truncated multibyte sequence counts as a single byte
    std::string bad = "\xF0\x9F";
    ASSERT_EQ(tui::text::utf8_len(bad, 0), (size_t)1);
}

TEST(textutil_wrap_respects_width_and_newlines) {
    auto lines = tui::text::wrap("the quick brown fox", 9);
    ASSERT_FALSE(lines.empty());
    for (const auto& l : lines)
        ASSERT(tui::text::display_cols(l) <= 9);

    auto para = tui::text::wrap("one\ntwo", 40);
    ASSERT_EQ(para.size(), (size_t)2);
    ASSERT_EQ(para[0], "one");
    ASSERT_EQ(para[1], "two");
}

TEST(textutil_wrap_strips_ansi_and_expands_tabs) {
    // ANSI color escape should be removed; tab becomes four spaces.
    auto lines = tui::text::wrap("\x1b[31mred\x1b[0m\tx", 80);
    ASSERT_EQ(lines.size(), (size_t)1);
    ASSERT_EQ(lines[0], "red    x");
}

TEST(textutil_to_wide_decodes_codepoints) {
    std::wstring w = tui::text::to_wide("a\xF0\x9F\x98\x80");
    ASSERT_EQ(w.size(), (size_t)2);
    ASSERT_EQ((long)w[0], (long)'a');
    ASSERT_EQ((long)w[1], (long)0x1F600);
}

// ---------------------------------------------------------------------------
// TUI command palette (slash-command filtering / completion — no ncurses)
// ---------------------------------------------------------------------------

static std::vector<tui::palette::Command> palette_fixture() {
    return {
        {"help", {"?", "h"}, "[command]", "list commands", nullptr},
        {"window", {"win", "w"}, "new|close", "manage windows", nullptr},
        {"save", {}, "", "persist conversation", nullptr},
        {"quit", {"exit", "q"}, "", "exit", nullptr},
    };
}

TEST(palette_token_and_arg_detection) {
    ASSERT_EQ(tui::palette::token("/wi"), "wi");
    ASSERT_EQ(tui::palette::token("/window new"), "window");
    ASSERT_EQ(tui::palette::token(""), "");
    ASSERT_EQ(tui::palette::token("plain"), "");
    ASSERT_TRUE(tui::palette::wants_open("/x"));
    ASSERT_FALSE(tui::palette::wants_open("x"));
    ASSERT_TRUE(tui::palette::has_arg("/window new"));
    ASSERT_FALSE(tui::palette::has_arg("/window"));
}

TEST(palette_filter_matches_name_and_alias) {
    auto cmds = palette_fixture();
    ASSERT_EQ(tui::palette::filter(cmds, "").size(), (size_t)4);   // all
    ASSERT_EQ(tui::palette::filter(cmds, "w").size(), (size_t)1);  // window
    ASSERT_EQ(tui::palette::filter(cmds, "win").front()->name, "window");
    ASSERT_EQ(tui::palette::filter(cmds, "q").front()->name, "quit");  // alias
    ASSERT_TRUE(tui::palette::filter(cmds, "zzz").empty());
}

TEST(palette_find_by_name_or_alias) {
    auto cmds = palette_fixture();
    ASSERT_TRUE(tui::palette::find(cmds, "help") != nullptr);
    ASSERT_EQ(tui::palette::find(cmds, "exit")->name, "quit");
    ASSERT_TRUE(tui::palette::find(cmds, "nope") == nullptr);
}

TEST(palette_complete_prefix_and_selection) {
    auto cmds = palette_fixture();
    // No selection: extend to the longest common prefix of the matches'
    // names (here just "window", no trailing space until it is exact).
    ASSERT_EQ(tui::palette::complete(cmds, "/wi", -1), "/window");
    // Exact unique name: append a space, ready for args.
    ASSERT_EQ(tui::palette::complete(cmds, "/window", -1), "/window ");
    // Selection index picks that match directly.
    ASSERT_EQ(tui::palette::complete(cmds, "/", 3), "/quit ");
    // No match: input unchanged.
    ASSERT_EQ(tui::palette::complete(cmds, "/zzz", -1), "/zzz");
}

TEST(palette_usage_and_common_prefix) {
    tui::palette::Command c{"window", {}, "new|close", "manage", nullptr};
    ASSERT_EQ(tui::palette::usage(c), "/window new|close");
    tui::palette::Command bare{"save", {}, "", "persist", nullptr};
    ASSERT_EQ(tui::palette::usage(bare), "/save");
    ASSERT_EQ(tui::palette::common_prefix({"send", "set", "sever"}), "se");
    ASSERT_EQ(tui::palette::common_prefix({}), "");
}

TEST(config_load_key_value) {
    std::string path = "/tmp/amber_cfg_test.txt";
    {
        std::ofstream f(path);
        f << "# comment\n";
        f << "model=\"my-model\"\n";
        f << "api_base=http://example:1234/v1\n";
        f << "max_tool_iterations=5\n";
        f << "temperature=0.9\n";
        f << "stream=false\n";
    }
    agent::Config c;
    c.load(path);
    ASSERT_EQ(c.model, "my-model");
    ASSERT_EQ(c.api_base, "http://example:1234/v1");
    ASSERT_EQ(c.max_tool_iterations, 5);
    ASSERT_EQ(c.temperature, 0.9);
    ASSERT_FALSE(c.stream);
    std::remove(path.c_str());
}

// Mirrors what the TUI F10 "save settings" writes for a llama.cpp server, and
// that an optional (possibly empty) token survives a load round-trip. This
// guards the settings-persistence contract used by the TUI.
TEST(config_save_settings_roundtrip) {
    std::string path = "/tmp/amber_settings_test.conf";
    {
        std::ofstream f(path);
        f << "# amber settings\n";
        f << "api_base=http://localhost:8080/v1\n";
        f << "api_key=sk-test-token\n";
        f << "model=llama-3.2-3b-instruct\n";
    }
    agent::Config c;
    c.load(path);
    ASSERT_EQ(c.api_base, "http://localhost:8080/v1");
    ASSERT_EQ(c.api_key, "sk-test-token");
    ASSERT_EQ(c.model, "llama-3.2-3b-instruct");

    // Optional/blank token is also preserved as empty.
    {
        std::ofstream f(path);
        f << "api_base=http://localhost:8080/v1\n";
        f << "api_key=\n";
        f << "model=llama-3.2-3b-instruct\n";
    }
    agent::Config c2;
    c2.load(path);
    ASSERT_EQ(c2.api_key, "");
    std::remove(path.c_str());
}

TEST(config_missing_file_is_noop) {
    agent::Config c;
    c.model = "keep";
    c.load("/nonexistent/path/xyz.cfg");
    ASSERT_EQ(c.model, "keep");
}

// ---------------------------------------------------------------------------
// Tool registry
// ---------------------------------------------------------------------------

TEST(registry_register_and_find) {
    agent::ToolRegistry r;
    agent::register_default_tools(r);
    ASSERT_FALSE(r.empty());
    ASSERT_EQ(r.tools().size(), 3u);
    ASSERT(r.find("read") != nullptr);
    ASSERT(r.find("write") != nullptr);
    ASSERT(r.find("search") != nullptr);
    ASSERT(r.find("nonexistent") == nullptr);
}

TEST(registry_schema_shape) {
    agent::ToolRegistry r;
    agent::register_default_tools(r);
    agent::json s = r.schema();
    ASSERT(s.is_array());
    ASSERT_EQ(s.size(), 3u);
    for (const auto& t : s) {
        ASSERT(t.contains("type"));
        ASSERT_EQ(t["type"], "function");
        ASSERT(t["function"].contains("name"));
        ASSERT(t["function"].contains("description"));
        ASSERT(t["function"]["parameters"].contains("properties"));
    }
}

// ---------------------------------------------------------------------------
// Prompt loading + tool advertising
// ---------------------------------------------------------------------------

TEST(prompt_missing_file_empty) {
    ASSERT_EQ(agent::load_prompt("/does/not/exist.md"), "");
}

TEST(prompt_loads_existing) {
    std::string path = "/tmp/amber_prompt_test.md";
    {
        std::ofstream f(path);
        f << "# Title\nbody text\n";
    }
    ASSERT_EQ(agent::load_prompt(path), "# Title\nbody text\n");
    std::remove(path.c_str());
}

TEST(prompt_render_tools_markdown_lists_all) {
    agent::ToolRegistry r;
    agent::register_default_tools(r);
    std::string md = agent::render_tools_markdown(r);
    ASSERT(md.find("## Available Tools") != std::string::npos);
    ASSERT(md.find("`read`") != std::string::npos);
    ASSERT(md.find("`write`") != std::string::npos);
    ASSERT(md.find("`search`") != std::string::npos);
    ASSERT(md.find("path") != std::string::npos);   // a known parameter
}

// ---------------------------------------------------------------------------
// read tool (pagination)
// ---------------------------------------------------------------------------

TEST(read_tool_basic_and_pagination) {
    agent::Workspace::set_root("/tmp");
    std::string path = "/tmp/amber_read_test.txt";
    {
        std::ofstream f(path);
        for (int i = 1; i <= 10; ++i) f << "line " << i << "\n";
    }
    auto tool = agent::make_read_tool();
    auto r = tool->execute({{"path", path}, {"offset", 1}, {"limit", 3}});
    ASSERT_TRUE(r.ok);
    ASSERT(r.output.find("1:\tline 1") != std::string::npos);
    ASSERT(r.output.find("3:\tline 3") != std::string::npos);
    ASSERT(r.output.find("remaining") != std::string::npos);

    // page 2
    auto r2 = tool->execute({{"path", path}, {"offset", 4}, {"limit", 3}});
    ASSERT_TRUE(r2.ok);
    ASSERT(r2.output.find("4:\tline 4") != std::string::npos);
    ASSERT(r2.output.find("6:\tline 6") != std::string::npos);

    // past end reports EOF, no remaining
    auto r3 = tool->execute({{"path", path}, {"offset", 9}, {"limit", 50}});
    ASSERT_TRUE(r3.ok);
    ASSERT(r3.output.find("end of file") != std::string::npos);
    std::remove(path.c_str());
}

TEST(read_tool_missing_path_errors) {
    auto tool = agent::make_read_tool();
    auto r = tool->execute({{"limit", 5}});   // no path
    ASSERT_FALSE(r.ok);
    ASSERT_FALSE(r.error.empty());
}

// ---------------------------------------------------------------------------
// write tool (patch style)
// ---------------------------------------------------------------------------

TEST(write_tool_create_then_patch) {
    agent::Workspace::set_root("/tmp");
    std::string path = "/tmp/amber_write_test.txt";
    std::remove(path.c_str());
    auto tool = agent::make_write_tool();

    auto r = tool->execute({{"path", path},
                            {"edits", {{{"old", ""}, {"new", "alpha\nbeta\n"}}}}});
    ASSERT_TRUE(r.ok);
    {
        std::ifstream f(path); std::stringstream ss; ss << f.rdbuf();
        ASSERT_EQ(ss.str(), "alpha\nbeta\n");
    }

    auto r2 = tool->execute({{"path", path},
                             {"edits", {{{"old", "beta"}, {"new", "gamma"}}}}});
    ASSERT_TRUE(r2.ok);
    {
        std::ifstream f(path); std::stringstream ss; ss << f.rdbuf();
        ASSERT_EQ(ss.str(), "alpha\ngamma\n");
    }
    std::remove(path.c_str());
}

TEST(write_tool_missing_old_fails) {
    agent::Workspace::set_root("/tmp");
    std::string path = "/tmp/amber_write_test2.txt";
    std::remove(path.c_str());
    auto tool = agent::make_write_tool();
    auto r = tool->execute({{"path", path},
                            {"edits", {{{"old", "nope"}, {"new", "x"}}}}});
    ASSERT_FALSE(r.ok);
    ASSERT(r.error.find("not found") != std::string::npos);
    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// workspace path confinement
// ---------------------------------------------------------------------------

TEST(workspace_confines_relative_and_rejects_escape) {
    agent::Workspace::set_root("/tmp/amber_ws");
    std::string resolved, err;

    ASSERT_TRUE(agent::Workspace::confine("a/b.txt", resolved, err));
    ASSERT_EQ(resolved, "/tmp/amber_ws/a/b.txt");

    ASSERT_TRUE(agent::Workspace::confine("./x/../y.txt", resolved, err));
    ASSERT_EQ(resolved, "/tmp/amber_ws/y.txt");

    ASSERT_FALSE(agent::Workspace::confine("../../etc/passwd", resolved, err));
    ASSERT_FALSE(err.empty());

    ASSERT_FALSE(agent::Workspace::confine("/etc/passwd", resolved, err));
    ASSERT_FALSE(err.empty());

    // sibling directory sharing a prefix must not be treated as inside
    ASSERT_FALSE(agent::Workspace::confine("/tmp/amber_ws2/x", resolved, err));
}

TEST(read_write_tools_reject_paths_outside_workspace) {
    agent::Workspace::set_root("/tmp/amber_ws_tools");
    run_cmd("mkdir -p /tmp/amber_ws_tools");

    auto rtool = agent::make_read_tool();
    auto rr = rtool->execute({{"path", "/etc/passwd"}});
    ASSERT_FALSE(rr.ok);
    ASSERT(rr.error.find("workspace") != std::string::npos);

    auto wtool = agent::make_write_tool();
    auto wr = wtool->execute({{"path", "../escape.txt"},
                              {"edits", {{{"old", ""}, {"new", "x"}}}}});
    ASSERT_FALSE(wr.ok);
    ASSERT(wr.error.find("workspace") != std::string::npos);
}

// ---------------------------------------------------------------------------
// search backends
// ---------------------------------------------------------------------------

namespace {
std::string make_search_tree() {
    std::string dir = "/tmp/amber_srch";
    std::string cmd = "rm -rf " + dir + " && mkdir -p " + dir + "/sub";
    run_cmd(cmd);
    {
        std::ofstream f(dir + "/a.cpp");
        f << "void register_default_tools() {}\nint helper() { return 1; }\n";
    }
    {
        std::ofstream f(dir + "/sub/b.cpp");
        f << "void register_default_tools() {}\n// unrelated content\n";
    }
    {
        std::ofstream f(dir + "/note.txt");
        f << "register_default_tools is the function we want to find\n";
    }
    return dir;
}
} // namespace

TEST(search_grep_backend) {
    std::string dir = make_search_tree();
    auto be = agent::make_grep_backend();
    auto hits = be->search("register_default_tools", dir, "*.cpp", 100);
    ASSERT_EQ(be->name(), "grep");
    ASSERT_FALSE(hits.empty());
    bool saw_a = false, saw_b = false;
    for (const auto& h : hits) {
        if (h.path.find("a.cpp") != std::string::npos) saw_a = true;
        if (h.path.find("b.cpp") != std::string::npos) saw_b = true;
        ASSERT(h.line_no > 0);
    }
    ASSERT(saw_a && saw_b);
    run_cmd("rm -rf " + dir);
}

TEST(search_grep_backend_resists_shell_injection) {
    std::string dir = make_search_tree();
    std::string sentinel = "/tmp/amber_pwned";
    run_cmd("rm -f " + sentinel);
    auto be = agent::make_grep_backend();
    // A query crafted to break out of the command if quoting were absent.
    auto hits = be->search("x'; touch " + sentinel + "; echo '", dir, "", 100);
    // The injected command must not have run.
    ASSERT_FALSE(access(sentinel.c_str(), F_OK) == 0);
    (void)hits;
    run_cmd("rm -rf " + dir + " " + sentinel);
}

TEST(search_semantic_backend_ranks_relevant) {
    std::string dir = make_search_tree();
    auto be = agent::make_semantic_backend();
    auto hits = be->search("register the default tools function", dir, "", 5);
    ASSERT_EQ(be->name(), "semantic");
    ASSERT_FALSE(hits.empty());
    // The line containing register_default_tools should rank at or near the top.
    bool top_has_target = hits[0].line.find("register_default_tools") != std::string::npos;
    ASSERT(top_has_target);
    ASSERT(hits[0].score > 0.0);
    run_cmd("rm -rf " + dir);
}

TEST(search_tool_mode_switch) {
    std::string dir = make_search_tree();
    auto tool = agent::make_search_tool();

    auto g = tool->execute({{"pattern", "register_default_tools"},
                            {"path", dir}, {"glob", "*.cpp"}, {"mode", "grep"}});
    ASSERT_TRUE(g.ok);
    ASSERT(g.output.find("[grep]") != std::string::npos);

    auto s = tool->execute({{"pattern", "register the default tools"},
                            {"path", dir}, {"mode", "semantic"}});
    ASSERT_TRUE(s.ok);
    ASSERT(s.output.find("[semantic]") != std::string::npos);
    run_cmd("rm -rf " + dir);
}

// ---------------------------------------------------------------------------
// Server auto-detection: /v1/models parsing (pure, no network)
// ---------------------------------------------------------------------------

TEST(probe_parse_llamacpp_models) {
    // Real llama.cpp /v1/models shape (trimmed): id + meta.n_ctx/n_ctx_train.
    std::string body = R"({"object":"list","data":[{"id":"Qwopus3.6-27B.gguf",)"
        R"("object":"model","owned_by":"llamacpp","meta":{"n_vocab":248320,)"
        R"("n_ctx":262144,"n_ctx_train":262144,"n_embd":5120}}]})";
    agent::ServerInfo info = agent::LLMClient::parse_models(body);
    ASSERT_TRUE(info.ok);
    ASSERT_EQ(info.model, "Qwopus3.6-27B.gguf");
    ASSERT_EQ(info.context_size, 262144);
    ASSERT_EQ(info.context_train, 262144);
}

TEST(probe_parse_models_array_fallback) {
    // Ollama-ish {"models":[{"name":..,"n_ctx":..}]} fallback shape.
    std::string body =
        R"({"models":[{"name":"llama-3.2-3b","n_ctx":8192}]})";
    agent::ServerInfo info = agent::LLMClient::parse_models(body);
    ASSERT_TRUE(info.ok);
    ASSERT_EQ(info.model, "llama-3.2-3b");
    ASSERT_EQ(info.context_size, 8192);
}

TEST(probe_parse_models_malformed_is_not_ok) {
    ASSERT_FALSE(agent::LLMClient::parse_models("not json").ok);
    ASSERT_FALSE(agent::LLMClient::parse_models("{}").ok);
    ASSERT_FALSE(agent::LLMClient::parse_models(R"({"data":[]})").ok);
}

// ---------------------------------------------------------------------------
// Status-bar rendering math (pure, no ncurses / no network)
// ---------------------------------------------------------------------------

TEST(statusbar_kfmt) {
    ASSERT_EQ(agent::bar::kfmt(-1), "?");
    ASSERT_EQ(agent::bar::kfmt(0), "0");
    ASSERT_EQ(agent::bar::kfmt(512), "512");
    ASSERT_EQ(agent::bar::kfmt(999), "999");
    ASSERT_EQ(agent::bar::kfmt(5000), "5.0k");
    ASSERT_EQ(agent::bar::kfmt(1500), "1.5k");
    ASSERT_EQ(agent::bar::kfmt(128000), "128k");
}

TEST(statusbar_pressure_thresholds) {
    ASSERT(agent::bar::pressure(0.0) == agent::bar::Pressure::Ok);
    ASSERT(agent::bar::pressure(0.59) == agent::bar::Pressure::Ok);
    ASSERT(agent::bar::pressure(0.60) == agent::bar::Pressure::Warn);
    ASSERT(agent::bar::pressure(0.85) == agent::bar::Pressure::Warn);
    ASSERT(agent::bar::pressure(0.851) == agent::bar::Pressure::Crit);
    ASSERT(agent::bar::pressure(1.0) == agent::bar::Pressure::Crit);
}

TEST(statusbar_gauge_fill_cells) {
    // Empty and full extremes.
    ASSERT_EQ(agent::bar::gauge_full_cells(0.0, 10), 0);
    ASSERT_EQ(agent::bar::gauge_full_cells(1.0, 10), 10);
    // Half fill of 10 cells = 5 full cells.
    ASSERT_EQ(agent::bar::gauge_full_cells(0.5, 10), 5);
    // Clamps out-of-range fractions.
    ASSERT_EQ(agent::bar::gauge_full_cells(-0.5, 10), 0);
    ASSERT_EQ(agent::bar::gauge_full_cells(2.0, 10), 10);
    ASSERT_EQ(agent::bar::gauge_full_cells(0.5, 0), 0);
}

TEST(statusbar_gauge_bar_glyphs) {
    // Empty bar is all light-shade track (\u2591), one per cell (3 bytes each).
    std::string empty = agent::bar::gauge_bar(0.0, 4);
    ASSERT_EQ(empty, "\u2591\u2591\u2591\u2591");
    // Full bar is all full blocks (\u2588).
    std::string full = agent::bar::gauge_bar(1.0, 4);
    ASSERT_EQ(full, "\u2588\u2588\u2588\u2588");
    // Half of 4 cells: two full blocks then two empty.
    std::string half = agent::bar::gauge_bar(0.5, 4);
    ASSERT_EQ(half, "\u2588\u2588\u2591\u2591");
    // Degenerate width.
    ASSERT_EQ(agent::bar::gauge_bar(0.5, 0), "");
}

// ---------------------------------------------------------------------------
// LLM streaming SSE parse (integration via a tiny in-process HTTP server)
// ---------------------------------------------------------------------------

#if defined(__linux__)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {
// Serve one canned SSE response (a streamed tool call in two fragments), then
// close. Lets us exercise LLMClient::chat_stream including fragment merging
// without any external dependency.
int spawn_mock_sse(int port, std::string& body_out, const std::string& sse_override = "") {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int opt = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) { close(fd); return -1; }
    listen(fd, 1);
    body_out.clear();
    std::thread t([fd, sse_override]() {
        int c = accept(fd, nullptr, nullptr);
        if (c < 0) return;
        // read the request (headers + body) until we have it
        char buf[4096];
        std::string req;
        while (true) {
            int n = recv(c, buf, sizeof(buf) - 1, 0);
            if (n <= 0) break;
            req.append(buf, n);
            if (req.find("\r\n\r\n") != std::string::npos) break;
        }
        (void)req;
        std::string sse = !sse_override.empty() ? sse_override :
            std::string(
            "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":"
            "\"c1\",\"type\":\"function\",\"function\":{\"name\":\"search\","
            "\"arguments\":\"\"}}]}}]}\n\n"
            "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
            "\"function\":{\"arguments\":\"{\\\"pattern\\\":\\\"foo\\\",\\\"path\\\":\\\".\\\"}\"}}]}}]}\n\n"
            "data: [DONE]\n\n");
        std::string http =
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
            "Content-Length: " + std::to_string(sse.size()) + "\r\n\r\n" + sse;
        send(c, http.c_str(), http.size(), 0);
        // give client time to read
        usleep(200000);
        close(c);
    });
    t.detach();
    return fd;
}
} // namespace

TEST(llm_streaming_merges_tool_call_fragments) {
    std::string dummy;
    int srv = spawn_mock_sse(8911, dummy);
    ASSERT(srv >= 0);
    usleep(100000);  // let the listener bind

    agent::Config cfg;
    cfg.api_base = "http://127.0.0.1:8911/v1";
    cfg.stream = true;
    agent::LLMClient client(cfg);

    std::vector<std::string> tokens;
    agent::Message m = client.chat_stream({}, {},
        [&tokens](const agent::StreamChunk& ch) {
            if (!ch.done && !ch.delta.empty()) tokens.push_back(ch.delta);
        });

    ASSERT(m.tool_calls.is_array());
    ASSERT_EQ(m.tool_calls.size(), 1u);
    ASSERT_EQ(m.tool_calls[0]["function"]["name"], "search");
    std::string args = m.tool_calls[0]["function"]["arguments"].get<std::string>();
    agent::json parsed = agent::json::parse(args, nullptr, false);
    ASSERT_FALSE(parsed.is_discarded());
    ASSERT_EQ(parsed["pattern"], "foo");
    ASSERT_EQ(parsed["path"], ".");
    close(srv);
}

TEST(llm_streaming_inline_think_segmentation) {
    std::string dummy;
    // Content stream with inline <think> spanning fragments; answer follows.
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":\"<thi\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"nk>plan the\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\" answer</think>Hello \"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"world\"}}]}\n\n"
        "data: [DONE]\n\n";
    int srv = spawn_mock_sse(8912, dummy, sse);
    ASSERT(srv >= 0);
    usleep(100000);

    agent::Config cfg;
    cfg.api_base = "http://127.0.0.1:8912/v1";
    cfg.stream = true;
    agent::LLMClient client(cfg);

    std::string answer, reasoning;
    agent::Message m = client.chat_stream({}, {},
        [&](const agent::StreamChunk& ch) {
            if (ch.done) return;
            answer += ch.delta;
            reasoning += ch.reasoning;
        });

    ASSERT_EQ(m.content, "Hello world");
    ASSERT_EQ(m.reasoning, "plan the answer");
    ASSERT_EQ(answer, "Hello world");
    ASSERT_EQ(reasoning, "plan the answer");
    close(srv);
}

TEST(llm_streaming_reasoning_content_field) {
    std::string dummy;
    // Dedicated reasoning_content field (vLLM / llama.cpp deepseek format).
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"step one \"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"step two\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"done\"}}]}\n\n"
        "data: [DONE]\n\n";
    int srv = spawn_mock_sse(8913, dummy, sse);
    ASSERT(srv >= 0);
    usleep(100000);

    agent::Config cfg;
    cfg.api_base = "http://127.0.0.1:8913/v1";
    cfg.stream = true;
    agent::LLMClient client(cfg);

    agent::Message m = client.chat_stream({}, {},
        [](const agent::StreamChunk&) {});

    ASSERT_EQ(m.content, "done");
    ASSERT_EQ(m.reasoning, "step one step two");
    close(srv);
}

TEST(llm_streaming_captures_usage_stats) {
    std::string dummy;
    // Final include_usage chunk: usage present, empty choices[] (llama.cpp/vLLM).
    std::string sse =
        "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":4096,"
        "\"completion_tokens\":128,\"total_tokens\":4224}}\n\n"
        "data: [DONE]\n\n";
    int srv = spawn_mock_sse(8914, dummy, sse);
    ASSERT(srv >= 0);
    usleep(100000);

    agent::Config cfg;
    cfg.api_base = "http://127.0.0.1:8914/v1";
    cfg.stream = true;
    agent::LLMClient client(cfg);

    agent::Stats stats;
    agent::Message m = client.chat_stream({}, {},
        [](const agent::StreamChunk&) {}, &stats);

    ASSERT_EQ(m.content, "hi");
    ASSERT_TRUE(stats.valid);
    ASSERT_EQ(stats.prompt_tokens, 4096L);
    ASSERT_EQ(stats.completion_tokens, 128L);
    ASSERT_TRUE(stats.latency_ms >= 0);
    close(srv);
}
#endif // __linux__

// ---------------------------------------------------------------------------
// Session persistence
// ---------------------------------------------------------------------------

TEST(session_json_roundtrip_preserves_messages) {
    agent::Session s;
    s.model = "test-model";
    agent::Message sys; sys.role = "system"; sys.content = "be helpful";
    agent::Message u;   u.role = "user";     u.content = "hi\nthere";
    agent::Message a;   a.role = "assistant"; a.content = "hello";
    a.reasoning = "think first";
    s.messages = {sys, u, a};
    s.derive_title();
    ASSERT_EQ(s.title, "hi");

    agent::json j = s.to_json();
    agent::Session back = agent::Session::from_json(j);
    ASSERT_EQ(back.model, "test-model");
    ASSERT_EQ(back.messages.size(), 3u);
    ASSERT_EQ(back.messages[0].role, "system");
    ASSERT_EQ(back.messages[1].content, "hi\nthere");
    ASSERT_EQ(back.messages[2].reasoning, "think first");
}

TEST(session_store_save_load_list_delete) {
    std::string dir = "/tmp/amber_sessions_test";
    run_cmd("rm -rf " + dir);
    agent::SessionStore store(dir);

    agent::Session s1;
    agent::Message u; u.role = "user"; u.content = "first";
    s1.messages = {u};
    s1.derive_title();
    ASSERT_TRUE(store.save(s1));
    ASSERT_FALSE(s1.id.empty());
    ASSERT_TRUE(s1.updated_ms > 0);

    agent::Session loaded;
    ASSERT_TRUE(store.load(s1.id, loaded));
    ASSERT_EQ(loaded.messages.size(), 1u);
    ASSERT_EQ(loaded.messages[0].content, "first");

    usleep(2000);
    agent::Session s2;
    agent::Message u2; u2.role = "user"; u2.content = "second";
    s2.messages = {u2};
    s2.derive_title();
    ASSERT_TRUE(store.save(s2));

    auto metas = store.list();
    ASSERT_EQ(metas.size(), 2u);
    // Newest updated first.
    ASSERT_EQ(metas[0].id, s2.id);
    ASSERT_EQ(metas[0].message_count, 1);

    ASSERT_TRUE(store.remove(s1.id));
    ASSERT_EQ(store.list().size(), 1u);
    agent::Session gone;
    ASSERT_FALSE(store.load(s1.id, gone));
    run_cmd("rm -rf " + dir);
}

// ---------------------------------------------------------------------------
// Agent conversation memory (stateful across run() calls)
// ---------------------------------------------------------------------------

TEST(agent_retains_history_across_turns) {
    agent::Config cfg;
    agent::ToolRegistry reg;
    agent::Agent ag(cfg, reg);

    // Seed a prior conversation as if loaded from a session.
    agent::Message sys; sys.role = "system"; sys.content = "sys";
    agent::Message u;   u.role = "user";     u.content = "earlier";
    agent::Message a;   a.role = "assistant"; a.content = "reply";
    ag.set_history({sys, u, a});
    ASSERT_EQ(ag.history().size(), 3u);
    ASSERT_EQ(ag.history().front().role, "system");

    ag.reset();
    ASSERT_EQ(ag.history().size(), 0u);
}

// ---------------------------------------------------------------------------

int main() { return agent::test::run_all(); }
