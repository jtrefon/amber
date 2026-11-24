// Agent journeys through the real TUI binary: a pty, a loopback provider and
// real tools. The pty regression suite pins the interface; these journeys pin
// the thing a person does with it — prompt, tool, edit, answer — through main(),
// the plugin runtime and the tool registry.

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "agent/profiles.h"
#include "agent/skill_install.h"
#include "agent/workspace.h"
#include "agent/worktree.h"
#include "bench/scripted_endpoint.h"
#include "tests/minitest.h"
#include "tests/pty_harness.h"

namespace {

namespace fs = std::filesystem;
using agent::test::pty::require;
using agent::test::pty::Tui;
using agent::test::pty::wait_ready;

bench::EndpointReply tool_call(const std::string& id, const std::string& name,
                               const std::string& args) {
    bench::EndpointReply reply;
    agent::json call = {
        {"id", id}, {"type", "function"}, {"function", {{"name", name}, {"arguments", args}}}};
    reply.tool_calls = agent::json::array({call});
    return reply;
}

bench::EndpointReply answer(const std::string& text) {
    bench::EndpointReply reply;
    reply.content = text;
    return reply;
}

bool require_setup(bool ok, const std::string& what) {
    if (ok)
        return true;
    std::cerr << "FAIL: " << what << "\n";
    failed++;
    return false;
}

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// The checkpoint journey needs a real git repository: the user's HEAD and branch
// must survive every restore, and that is not observable on a bare directory.
bool run_git(const std::string& root, const std::string& args) {
    const std::string command = "git -C '" + root + "' " + args;
    return std::system(command.c_str()) == 0;
}

std::string git_output(const std::string& root, const std::string& args) {
    const std::string command = "git -C '" + root + "' " + args + " 2>/dev/null";
    FILE* pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr)
        return {};
    std::string out;
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr)
        out += buffer;
    ::pclose(pipe);
    return out;
}

// Child processes inherit these; the guard restores the developer's own values
// whether the journey passes, fails or returns early.
struct ScopedEnv {
    struct Entry {
        std::string name;
        bool had = false;
        std::string value;
    };
    std::vector<Entry> entries;

    explicit ScopedEnv(std::vector<std::pair<std::string, std::string>> vars) {
        for (auto& var : vars) {
            const char* old = std::getenv(var.first.c_str());
            entries.push_back({var.first, old != nullptr, old != nullptr ? old : ""});
            setenv(var.first.c_str(), var.second.c_str(), 1);
        }
    }
    ~ScopedEnv() {
        for (const auto& entry : entries) {
            if (entry.had)
                setenv(entry.name.c_str(), entry.value.c_str(), 1);
            else
                unsetenv(entry.name.c_str());
        }
    }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;
};

std::string first_token(const std::string& text) {
    const size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return {};
    const size_t end = text.find_first_of(" \t\r\n", begin);
    return text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

// A task id is `task_` plus [a-z0-9_-]; the list and start lines put a colon
// or whitespace after it.
std::string task_id_after(const std::string& text, const std::string& marker) {
    const size_t at = text.rfind(marker);
    if (at == std::string::npos)
        return {};
    size_t begin = at + marker.size();
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])))
        ++begin;
    size_t end = begin;
    while (end < text.size()) {
        const char c = text[end];
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'))
            break;
        ++end;
    }
    return text.substr(begin, end - begin);
}

// The first whitespace-delimited token after `marker`.
std::string token_after(const std::string& text, const std::string& marker) {
    const size_t at = text.rfind(marker);
    if (at == std::string::npos)
        return {};
    size_t pos = at + marker.size();
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])))
        ++pos;
    size_t end = pos;
    while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end])) &&
           text[end] != '"')
        ++end;
    return text.substr(pos, end - pos);
}

// Output produced since `mark`, so a marker that was painted earlier in the
// session cannot satisfy a later assertion.
bool wait_since(Tui& tui, size_t mark, const std::string& marker, int cap_ms = 15000) {
    for (int waited = 0; waited < cap_ms; waited += agent::test::pty::kPumpMs) {
        tui.pump(agent::test::pty::kPumpMs);
        if (agent::test::pty::text_since(tui, mark).find(marker) != std::string::npos)
            return true;
    }
    return false;
}

// A command that paints a long transcript is still writing when its marker
// lands. Input queued into a pty the app is not draining can lose its tail, so
// wait until the screen has been quiet for a few slices before the next write.
void settle_output(Tui& tui) {
    int quiet = 0;
    while (quiet < 4) {
        const size_t before = tui.raw.size();
        tui.pump(agent::test::pty::kPumpMs);
        if (tui.raw.size() == before)
            ++quiet;
        else
            quiet = 0;
    }
}

// Send one slash command and wait for its own output. The raw stream is
// cleared first so a marker painted by an earlier command cannot satisfy this
// assertion; the app still repaints only what changed, so the marker has to be
// one this command is the first to print.
bool run_command(Tui& tui, const std::string& line, const std::string& marker, int cap_ms = 15000) {
    tui.raw.clear();
    // Ctrl-G sends the line as typed. Enter while the completion drawer is open
    // dispatches the highlighted row instead, so a bare namespace (/get) would
    // run its first child.
    tui.send(line + "\x07");
    if (!tui.wait_for(marker, cap_ms))
        return false;
    settle_output(tui);
    return true;
}

bool mouse_sgr(const Tui& tui) {
    return tui.raw.find("\x1b[?1006") != std::string::npos;
}

bool mouse_enabled(const Tui& tui) {
    return tui.raw.find("\x1b[?100") != std::string::npos;
}

// A real click is press + release: ncurses only arms the next press after the
// release, so a bare press opens the menu once and swallows every later click.
void send_click(Tui& tui, bool sgr, int x, int y) {
    if (sgr) {
        const std::string at = ";" + std::to_string(x + 1) + ";" + std::to_string(y + 1);
        tui.send("\x1b[<0" + at + "M");
        tui.send("\x1b[<0" + at + "m");
        return;
    }
    tui.send(std::string("\x1b[M") + static_cast<char>(32) + static_cast<char>(32 + x) +
             static_cast<char>(32 + y));
    tui.send(std::string("\x1b[M") + static_cast<char>(32 + 3) + static_cast<char>(32 + x) +
             static_cast<char>(32 + y));
}

bool write_text_file(const std::string& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    return !ec && agent::test::pty::write_file(path, text);
}

bool run_shell(const std::string& command) {
    return std::system(command.c_str()) == 0;
}

// A local skill pack: the installer reads a .tar.gz whose single top-level
// directory holds SKILL.md. Built here so the journey never reaches the network.
bool make_skill_pack(const std::string& root, const std::string& name) {
    const std::string packs = root + "/.amber/packs";
    const std::string dir = packs + "/" + name;
    return write_text_file(dir + "/SKILL.md", "---\nname: " + name +
                                                  "\ndescription: Local pack fixture.\n---\n\n"
                                                  "A locally installed journey fixture.\n") &&
           run_shell("tar -czf '" + root + "/.amber/" + name + ".tar.gz' -C '" + packs + "' '" +
                     name + "'");
}

bool make_plugin_pack(const std::string& root) {
    const std::string packs = root + "/.amber/packs";
    const std::string dir = packs + "/fake";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return !ec &&
           fs::copy_file("tests/plugins/fake_manifest.json", dir + "/manifest.json",
                         fs::copy_options::overwrite_existing, ec) &&
           !ec &&
           fs::copy_file("tests/plugins/fake_plugin.py", dir + "/fake_plugin.py",
                         fs::copy_options::overwrite_existing, ec) &&
           !ec && ::chmod((dir + "/fake_plugin.py").c_str(), 0755) == 0 &&
           run_shell("tar -czf '" + root + "/.amber/fake-plugin.tar.gz' -C '" + dir +
                     "' manifest.json fake_plugin.py");
}

// The management fixture: a learned memory, a second provider, and the packs.
bool prepare_management_fixture(const std::string& root, const std::string& api_base) {
    const std::string experience = agent::Workspace::state_dir() + "/experience.json";
    std::error_code ec;
    fs::create_directories(fs::path(experience).parent_path(), ec);
    if (ec)
        return false;
    if (!write_text_file(experience, R"({"version":1,"memories":[{"id":"m1","name":"proj-name",)"
                                     R"("content":"project uses make","tags":["build"],)"
                                     R"("evidence":3,"last_confirm_turn":1,"score":0,)"
                                     R"("promoted":true}],"skills":[]})"))
        return false;
    const std::string providers = agent::global_config_dir() + "/providers";
    fs::create_directories(providers, ec);
    return !ec &&
           write_text_file(providers + "/custom.conf",
                           "provider=custom\napi_base=" + api_base +
                               "\nflavor=openai\nmodel=journey-test\n") &&
           write_text_file(providers + "/temp.conf", "provider=temp\napi_base=" + api_base +
                                                         "\nflavor=openai\napi_key=sk-test\n"
                                                         "model=journey-temp\n") &&
           make_plugin_pack(root);
}

// The checkpoint fixture a managed skill needs: the install directory plus the
// ledger entry that records the remote revision and the local content hash.
bool prepare_managed_skill(const std::string& root) {
    const std::string skills = root + "/.amber/skills";
    const std::string dir = skills + "/tdd";
    const std::string body = "---\nname: tdd\ndescription: Managed tdd fixture.\n---\n\nbody one\n";
    if (!write_text_file(dir + "/SKILL.md", body))
        return false;
    const std::string local_hash = agent::skill_content_hash(body);
    const std::string ledger = "{\"version\":1,\"entries\":{\"tdd\":{\"id\":\"acme/skills/tdd\","
                               "\"source\":\"acme/skills\",\"slug\":\"tdd\",\"ref\":\"main\","
                               "\"hash\":\"old-hash\",\"local_hash\":\"" +
                               local_hash +
                               "\",\"category\":\"engineering\",\"note\":\"\",\"audit\":\"pass\","
                               "\"installed_at_ms\":1,\"files\":1,\"bytes\":" +
                               std::to_string(body.size()) + ",\"enabled\":true}}}";
    return write_text_file(skills + "/.lock.json", ledger);
}

// A loopback GitHub/skills mirror: only the paths the update check reads. The
// tree names a revision the ledger has never seen, so the remote is ahead.
std::unique_ptr<bench::ScriptedEndpoint> make_skills_mirror() {
    return std::make_unique<bench::ScriptedEndpoint>([](const bench::EndpointRequest& request) {
        bench::EndpointReply reply;
        reply.content_type = "text/plain";
        if (request.path == "/api/search" || request.path == "/api/v1/skills/search") {
            reply.content_type = "application/json";
            reply.body = R"({"skills":[{"id":"acme/skills/tdd","name":"tdd",)"
                         R"("source":"acme/skills","installs":1234}],"count":1})";
        } else if (request.path == "/repos/acme/skills") {
            reply.content_type = "application/json";
            reply.body = R"({"default_branch":"main","license":{"spdx_id":"MIT"}})";
        } else if (request.path == "/repos/acme/skills/git/trees/main") {
            reply.content_type = "application/json";
            reply.body =
                R"({"truncated":false,"tree":[)"
                R"({"path":"skills/tdd/SKILL.md","type":"blob","size":64,"sha":"new-hash"},)"
                R"({"path":"skills/tdd/reference.md","type":"blob","size":8,"sha":"sha-ref"}]})";
        } else if (request.path == "/acme/skills/main/skills/tdd/SKILL.md") {
            reply.body = "---\nname: tdd\ndescription: Managed tdd fixture v2.\n---\n\nbody two\n";
        } else if (request.path == "/acme/skills/main/skills/tdd/reference.md") {
            reply.body = "ref one\n";
        } else {
            reply.handled = false;
        }
        return reply;
    });
}

// One journey owns an isolated root, a loopback endpoint and the pty session.
struct Journey {
    std::string name;
    std::string root;
    std::unique_ptr<bench::ScriptedEndpoint> endpoint;
    Tui tui;

    ~Journey() { tui.stop(); }

    // `prepare` runs after the isolated root exists and before the app starts,
    // for fixtures the window scans once at startup (skills, MCP servers).
    using Prepare = std::function<void(const std::string& root)>;

    bool start(const std::string& binary, const std::string& journey_name,
               std::deque<bench::EndpointReply> script, const std::string& extra_settings = "",
               const Prepare& prepare = {}, bool root_api_base = false) {
        return start(binary, journey_name,
                     std::make_unique<bench::ScriptedEndpoint>(std::move(script)), extra_settings,
                     prepare, root_api_base, false);
    }

    bool start(const std::string& binary, const std::string& journey_name,
               bench::EndpointResponder responder, const std::string& extra_settings = "",
               const Prepare& prepare = {}, bool root_api_base = false) {
        return start(binary, journey_name,
                     std::make_unique<bench::ScriptedEndpoint>(std::move(responder)),
                     extra_settings, prepare, root_api_base, false);
    }

private:
    bool start(const std::string& binary, const std::string& journey_name,
               std::unique_ptr<bench::ScriptedEndpoint> server, const std::string& extra_settings,
               const Prepare& prepare, bool root_api_base, bool already_started) {
        name = journey_name;
        root = agent::test::pty::make_isolated_root();
        if (root.empty())
            return false;
        if (prepare)
            prepare(root);
        endpoint = std::move(server);
        if (!already_started && !endpoint->start())
            return false;
        // The versioned dialects (Anthropic, Gemini) add their own /v1./v1beta
        // segment, so their api_base is the bare root.
        const std::string base = root_api_base ? endpoint->root_url() : endpoint->base_url();
        const std::string settings =
            "api_base=" + base + "\nmodel=journey-test\nstream=0\n" + extra_settings;
        if (!agent::test::pty::write_file(agent::Workspace::settings_path(), settings))
            return false;
        tui.name = name;
        return tui.start(binary, root);
    }

public:
    // A fixture that must know the endpoint URL before startup (a project pin
    // reads its profile file on the first paint) seeds it against an endpoint
    // started by the caller, then hands ownership to the journey.
    bool start_with(const std::string& binary, const std::string& journey_name,
                    std::unique_ptr<bench::ScriptedEndpoint> server,
                    const std::string& extra_settings = "", const Prepare& prepare = {},
                    bool root_api_base = false) {
        return start(binary, journey_name, std::move(server), extra_settings, prepare,
                     root_api_base, true);
    }

    std::string file(const std::string& file_name) const {
        return read_text(fs::path(root) / file_name);
    }

    bool finish(const std::string& what) {
        tui.stop();
        if (!require(tui.exit_ready_seen(), what + ": no exit-ready marker", tui))
            return false;
        return require(tui.exit_code == 0, what + ": exit code != 0", tui);
    }
};

void journey_read_edit_verify(const std::string& binary) {
    Journey journey;
    if (!require(journey.start(
                     binary, "read-edit-verify",
                     {tool_call("c1", "read", R"({"path":"a.txt"})"),
                      tool_call("c2", "write",
                                R"({"path":"a.txt","edits":[{"old":"hello","new":"hi"}]})"),
                      tool_call("c3", "read", R"({"path":"a.txt"})"), answer("JOURNEY-ONE-DONE")}),
                 "the TUI did not start", journey.tui))
        return;
    agent::test::pty::write_file(journey.root + "/a.txt", "hello\n");
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;
    journey.tui.send("Read a.txt, change hello to hi, read it back. JOURNEY-ONE\r");
    if (!require(journey.tui.wait_for("JOURNEY-ONE-DONE", 30000), "the final answer is missing",
                 journey.tui))
        return;
    require(journey.file("a.txt") == "hi\n", "the edit did not reach a.txt", journey.tui);
    require(journey.endpoint->chat_calls() == 4, "expected four chat requests", journey.tui);
    journey.finish("read-edit-verify");
}

void journey_approval_denied_falls_back(const std::string& binary) {
    Journey journey;
    journey.start(
        binary, "approval-denied",
        {tool_call("c1", "bash", R"({"command":"echo bad > /tmp/amber-pty-journey-two"})"),
         tool_call("c2", "write", R"({"path":"a.txt","edits":[{"old":"hello","new":"good"}]})"),
         answer("JOURNEY-TWO-DONE")});
    agent::test::pty::write_file(journey.root + "/a.txt", "hello\n");
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;
    journey.tui.send("Change a.txt to good. JOURNEY-TWO\r");
    if (!require(journey.tui.wait_for("Approve action?", 20000), "the approval dialog never opened",
                 journey.tui))
        return;
    journey.tui.send("4"); // Always deny
    if (!require(journey.tui.wait_for("approval: always deny", 10000),
                 "the denial was not recorded", journey.tui))
        return;
    if (!require(journey.tui.wait_for("JOURNEY-TWO-DONE", 30000),
                 "the run did not recover after the denial", journey.tui))
        return;
    require(journey.file("a.txt") == "good\n", "the fallback write did not land", journey.tui);
    journey.finish("approval-denied");
}

void journey_second_turn_carries_context(const std::string& binary) {
    int call = 0;
    Journey journey;
    if (!require(journey.start(binary, "context-carry",
                               [&call](const bench::EndpointRequest& request) {
                                   bench::EndpointReply reply;
                                   if (request.path != "/v1/chat/completions") {
                                       reply.handled = false; // the endpoint's own /v1/models
                                       return reply;
                                   }
                                   ++call;
                                   if (call == 1)
                                       return tool_call("c1", "read", R"({"path":"a.txt"})");
                                   if (call == 2)
                                       return answer("FIRST-TURN-DONE");
                                   reply.content = request.body.find("hello") != std::string::npos
                                                       ? "CONTEXT-CARRIED"
                                                       : "CONTEXT-LOST";
                                   return reply;
                               }),
                 "the TUI did not start", journey.tui))
        return;
    agent::test::pty::write_file(journey.root + "/a.txt", "hello\n");
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;
    journey.tui.send("Read a.txt. FIRST-TURN\r");
    if (!require(journey.tui.wait_for("FIRST-TURN-DONE", 30000), "the first turn did not finish",
                 journey.tui))
        return;
    journey.tui.send("What did a.txt contain? SECOND-TURN\r");
    if (!require(journey.tui.wait_for("CONTEXT-CARRIED", 30000),
                 "the second turn did not see the first turn's tool result", journey.tui))
        return;
    require(journey.endpoint->chat_calls() == 3, "expected three chat requests", journey.tui);
    journey.finish("context-carry");
}

void journey_approval_allowed(const std::string& binary) {
    const std::string marker = "/tmp/amber-tui-journey-approved";
    std::error_code ec;
    fs::remove(marker, ec);
    Journey journey;
    if (!require(journey.start(
                     binary, "approval-allowed",
                     {tool_call("c1", "bash",
                                R"({"command":"echo approved > /tmp/amber-tui-journey-approved"})"),
                      answer("JOURNEY-APPROVED-DONE")}),
                 "the TUI did not start", journey.tui))
        return;
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;
    journey.tui.send("Run the marker command. JOURNEY-APPROVE\r");
    if (!require(journey.tui.wait_for("Approve action?", 20000), "the approval dialog never opened",
                 journey.tui))
        return;
    journey.tui.send("1"); // Allow once
    if (!require(journey.tui.wait_for("approval: allowed once", 10000),
                 "the approval was not accepted", journey.tui))
        return;
    if (!require(journey.tui.wait_for("JOURNEY-APPROVED-DONE", 30000),
                 "the approved run did not finish", journey.tui))
        return;
    require(fs::exists(marker), "the approved command did not write its marker", journey.tui);
    fs::remove(marker, ec);
    journey.finish("approval-allowed");
}

void journey_cancel_slow_tool(const std::string& binary) {
    Journey journey;
    if (!require(journey.start(
                     binary, "cancel-slow-tool",
                     {tool_call("c1", "bash", R"({"command":"sleep 5; echo x > partial.txt"})"),
                      answer("AFTER-CANCEL-DONE")}),
                 "the TUI did not start", journey.tui))
        return;
    journey.tui.pump(1000);
    journey.tui.send("Run the slow command. JOURNEY-CANCEL\r");
    journey.tui.pump(1000);
    if (journey.tui.wait_for("Approve action?", 5000))
        journey.tui.send("1"); // Allow once
    if (!require(journey.tui.wait_for("bash", 15000), "the slow tool never started", journey.tui))
        return;
    journey.tui.send("/stop\r");
    if (!require(journey.tui.wait_for("stop requested", 8000), "the stop was not requested",
                 journey.tui))
        return;
    // The cancel ends the run but not the session: the prompt accepts a new
    // turn, which is the observable proof the worker returned. The tool result
    // itself may have scrolled past the failure dump, so the next turn is the
    // signal; the partial file is checked afterwards.
    journey.tui.send("Run another turn after the cancel.\r");
    if (!require(journey.tui.wait_for("AFTER-CANCEL-DONE", 15000),
                 "the TUI did not accept a turn after the cancel", journey.tui))
        return;
    require(!fs::exists(fs::path(journey.root) / "partial.txt"),
            "the cancelled tool left a partial file", journey.tui);
    journey.finish("cancel-slow-tool");
}

void journey_background_tab_keeps_its_result(const std::string& binary) {
    Journey journey;
    bench::EndpointReply slow = answer("BACKGROUND-TAB-DONE");
    slow.delay_ms = 1500;
    if (!require(journey.start(binary, "background-tab", {slow}), "the TUI did not start",
                 journey.tui))
        return;
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;
    journey.tui.send("/window new noworkdir\r");
    journey.tui.pump(800);
    journey.tui.send("/window rename worker\r");
    journey.tui.pump(500);
    journey.tui.send("Run the background task. BACKGROUND-TAB\r");
    journey.tui.pump(300);
    if (!require(journey.tui.wait_for("> Run the background task", 8000),
                 "the worker prompt was not submitted", journey.tui))
        return;
    journey.tui.send("/window set 1\r");
    journey.tui.pump(500);
    const size_t mark = journey.tui.raw.size();
    journey.tui.pump(2500); // the endpoint delay has elapsed; the worker is done
    const std::string first_tab = agent::test::pty::text_since(journey.tui, mark);
    if (!require(first_tab.find("BACKGROUND-TAB-DONE") == std::string::npos,
                 "the background result leaked into the first tab", journey.tui))
        return;
    journey.tui.send("/window set 2\r");
    if (!require(journey.tui.wait_for("BACKGROUND-TAB-DONE", 10000),
                 "the background result did not land in its own tab", journey.tui))
        return;
    journey.finish("background-tab");
}

// #621: switching tabs while an agent is still working must leave the UI
// live. No mouse input is sent anywhere in this journey: if a wheel tick is
// what restarts rendering, the prompt echo below never arrives.
void journey_tab_switch_stays_live(const std::string& binary) {
    Journey journey;
    bench::EndpointReply slow = answer("SWITCH-TAB-DONE");
    slow.delay_ms = 4000;
    if (!require(journey.start(binary, "tab-switch-live", {slow}), "the TUI did not start",
                 journey.tui))
        return;
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;

    journey.tui.send("\x0e"); // Ctrl+N: a second session tab, no drawer left open
    if (!require(journey.tui.wait_for(" 2 chat   \u00d7", 8000), "Ctrl+N did not add a tab",
                 journey.tui))
        return;
    journey.tui.send("Run the slow task. SWITCH-TAB\r");
    if (!require(journey.tui.wait_for("> Run the slow task", 8000), "the prompt was not submitted",
                 journey.tui))
        return;

    // The endpoint is still sleeping: switch away by clicking the first tab,
    // exactly the path in the report. xterm's SGR mouse mode is enabled by the
    // app; a press and a release are both required.
    if (!require(journey.tui.raw.find("\x1b[?1006") != std::string::npos,
                 "the app never enabled SGR mouse reporting", journey.tui))
        return;
    journey.tui.send("\x1b[<0;5;1M"); // SGR columns are 1-based: x=4
    journey.tui.send("\x1b[<0;5;1m");
    journey.tui.pump(500);
    const size_t list_mark = journey.tui.raw.size();
    journey.tui.send("/window list");
    journey.tui.pump(250);
    journey.tui.send("\x07");
    journey.tui.pump(500);
    if (!require(agent::test::pty::text_since(journey.tui, list_mark).find("1:amber*") !=
                     std::string::npos,
                 "clicking the tab did not switch sessions", journey.tui))
        return;
    journey.tui.send("liveness-probe");
    if (!require(journey.tui.wait_for("liveness-probe", 6000),
                 "the UI stopped accepting input after the tab switch", journey.tui))
        return;
    journey.tui.send("\x07"); // drop the probe before issuing another command

    journey.tui.send("/window set 2\r");
    if (!require(journey.tui.wait_for("SWITCH-TAB-DONE", 10000),
                 "the in-flight result was lost across the switch", journey.tui))
        return;
    journey.finish("tab-switch-live");
}

void journey_restart_resumes_session(const std::string& binary) {
    const std::string root = agent::test::pty::make_isolated_root();
    if (!require_setup(!root.empty(), "the isolated root was not created"))
        return;
    int call = 0;
    bench::ScriptedEndpoint endpoint([&call](const bench::EndpointRequest& request) {
        bench::EndpointReply reply;
        if (request.path != "/v1/chat/completions") {
            reply.handled = false;
            return reply;
        }
        ++call;
        if (call == 1)
            return tool_call("c1", "read", R"({"path":"a.txt"})");
        if (call == 2)
            return answer("RESTART-FIRST-DONE");
        reply.content = request.body.find("RESTART-FIRST-DONE") != std::string::npos
                            ? "RESTART-CONTEXT-CARRIED"
                            : "RESTART-CONTEXT-LOST";
        return reply;
    });
    if (!require_setup(endpoint.start(), "the endpoint did not start"))
        return;
    if (!require_setup(agent::test::pty::write_file(agent::Workspace::settings_path(),
                                                    "api_base=" + endpoint.base_url() +
                                                        "\nmodel=journey-test\nstream=0\n"),
                       "the settings were not written"))
        return;
    agent::test::pty::write_file(root + "/a.txt", "hello\n");
    const std::string workspace = fs::path(root).filename().string();
    {
        Tui first;
        first.name = "restart-resume (first)";
        if (!require(first.start(binary, root), "the first TUI did not start", first))
            return;
        if (!require(wait_ready(first, workspace), "the first UI never came up", first))
            return;
        first.send("Read a.txt. RESTART-FIRST\r");
        if (!require(first.wait_for("RESTART-FIRST-DONE", 30000), "the first turn did not finish",
                     first))
            return;
        first.stop();
        if (!require(first.exit_ready_seen(), "the first session did not announce its exit", first))
            return;
    }
    {
        Tui second;
        second.name = "restart-resume (second)";
        if (!require(second.start(binary, root), "the second TUI did not start", second))
            return;
        // The restored window is already painted; the startup marker and a
        // settle are enough (the prompt may be drawn as a diff of the old one).
        if (!require(second.wait_for("[" + workspace + "]\u2500", 20000),
                     "the second UI never came up", second))
            return;
        second.pump(1500);
        second.send("What did I ask you to read? RESTART-SECOND\r");
        if (!require(second.wait_for("RESTART-CONTEXT-CARRIED", 30000),
                     "the restarted session did not carry the first turn", second))
            return;
        second.stop();
        if (!require(second.exit_ready_seen(), "the second session did not announce its exit",
                     second))
            return;
    }
    endpoint.stop();
}

// The MCP fixture is a real stdio server, built next to the test tree.
std::string mcp_fixture() {
    std::error_code ec;
    const fs::path path = fs::current_path(ec) / "tests/fixtures/mcp_echo";
    return ec ? std::string() : path.string();
}

void journey_subagent_reports_back(const std::string& binary) {
    int child_requests = 0;
    Journey journey;
    if (!require(
            journey.start(
                binary, "subagent-worker",
                [&child_requests](const bench::EndpointRequest& request) {
                    bench::EndpointReply reply;
                    if (request.path != "/v1/chat/completions") {
                        reply.handled = false;
                        return reply;
                    }
                    std::string last_user;
                    for (const agent::Message& message : request.messages)
                        if (message.role == "user")
                            last_user = message.content;
                    if (last_user.find("JOURNEY-SUBAGENT-GOAL") != std::string::npos) {
                        ++child_requests;
                        reply.content = "SUBAGENT-REPORT-DONE: the workspace is clean";
                        return reply;
                    }
                    if (request.body.find("SUBAGENT-REPORT-DONE") != std::string::npos) {
                        reply.content = "PARENT-SAW-SUBAGENT";
                        return reply;
                    }
                    if (request.body.find("call_delegate") != std::string::npos) {
                        reply.content = "SUBAGENT-STARTED";
                        return reply;
                    }
                    return tool_call(
                        "call_delegate", "delegate",
                        R"({"goal":"JOURNEY-SUBAGENT-GOAL: inspect the workspace","context":"the workspace root","expect":"one short report","label":"journey-worker"})");
                }),
            "the TUI did not start", journey.tui))
        return;
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;
    journey.tui.send("Delegate the workspace report. JOURNEY-SUBAGENT\r");
    if (!require(journey.tui.wait_for("Approve action?", 20000),
                 "the delegation approval never opened", journey.tui))
        return;
    journey.tui.send("1"); // Allow the worker
    if (!require(journey.tui.wait_for("PARENT-SAW-SUBAGENT", 30000),
                 "the parent never saw the worker's report", journey.tui))
        return;
    require(child_requests == 1, "the worker made more than one request", journey.tui);
    journey.tui.send("/tasks list\r");
    if (!require(journey.tui.wait_for("journey-worker", 8000),
                 "the worker was not listed in /tasks", journey.tui))
        return;
    journey.finish("subagent-worker");
}

void journey_skill_activation(const std::string& binary) {
    int calls = 0;
    Journey journey;
    if (!require(journey.start(
                     binary, "skill-activation",
                     [&calls](const bench::EndpointRequest& request) {
                         bench::EndpointReply reply;
                         if (request.path != "/v1/chat/completions") {
                             reply.handled = false;
                             return reply;
                         }
                         ++calls;
                         if (calls == 1)
                             return tool_call("c1", "read_skill", R"({"name":"journey-skill"})");
                         reply.content =
                             request.body.find("JOURNEY-SKILL-BODY-MARKER") != std::string::npos
                                 ? "SKILL-ACTIVATED"
                                 : "SKILL-BODY-MISSING";
                         return reply;
                     },
                     "",
                     [](const std::string& root) {
                         const std::string dir = root + "/.amber/skills/journey-skill";
                         std::error_code ec;
                         fs::create_directories(dir, ec);
                         require_setup(
                             agent::test::pty::write_file(
                                 dir + "/SKILL.md",
                                 "---\nname: journey-skill\ndescription: Journey skill body "
                                 "check.\n---\n\nWhen active, the model has this marker: "
                                 "JOURNEY-SKILL-BODY-MARKER\n"),
                             "the journey skill was not written");
                     }),
                 "the TUI did not start", journey.tui))
        return;
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;
    journey.tui.send("Activate the journey skill. JOURNEY-SKILL\r");
    if (!require(journey.tui.wait_for("SKILL-ACTIVATED", 30000),
                 "the activated skill body never reached the model", journey.tui))
        return;
    require(calls == 2, "the activation did not take exactly two requests", journey.tui);
    journey.finish("skill-activation");
}

void journey_skills_management(const std::string& binary) {
    auto mirror = make_skills_mirror();
    if (!require_setup(mirror->start(), "the skills mirror did not start"))
        return;
    ScopedEnv env({{"AMBER_SKILLS_INDEX_URL", mirror->root_url()},
                   {"AMBER_GITHUB_API_URL", mirror->root_url()},
                   {"AMBER_GITHUB_RAW_URL", mirror->root_url()}});
    Journey journey;
    if (!require(journey.start(binary, "skills-manage", {answer("SKILLS-SESSION-READY")}, "",
                               [](const std::string& root) {
                                   require_setup(prepare_managed_skill(root) &&
                                                     make_skill_pack(root, "loose-skill"),
                                                 "the skills fixtures were not written");
                               }),
                 "the TUI did not start", journey.tui))
        return;
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;
    // The app opens on the read-only welcome window: a turn creates the chat
    // window and its agent, which the skills commands read through.
    journey.tui.send("Prepare the session.\r");
    if (!require(journey.tui.wait_for("SKILLS-SESSION-READY", 30000),
                 "the first turn did not start a chat window", journey.tui))
        return;
    settle_output(journey.tui);

    if (!require(run_command(journey.tui, "/skills list", "project \u00b7 authored \u00b7 tdd"),
                 "/skills list did not name the managed skill", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/skills show", "project \u00b7 authored \u00b7 tdd"),
                 "/skills show did not list the catalog", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/skills show no-such-skill",
                             "no skill matching 'no-such-skill'"),
                 "/skills show did not report an unknown name", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/skills sources", "github api"),
                 "/skills sources did "
                 "not name the GitHub "
                 "endpoint",
                 journey.tui))
        return;
    if (!require(run_command(journey.tui, "/skills browse", "type to search"),
                 "/skills browse did not open the explorer", journey.tui))
        return;
    journey.tui.send("q");
    if (!require(journey.tui.wait_for("skills closed", 10000), "q did not close the explorer",
                 journey.tui))
        return;
    if (!require(run_command(journey.tui, "/skills search tdd", "1 results"),
                 "/skills search did not reach the prefilled explorer", journey.tui))
        return;
    journey.tui.send("q");
    if (!require(journey.tui.wait_for("skills closed", 10000), "q did not close the explorer",
                 journey.tui))
        return;
    if (!require(run_command(journey.tui, "/skills install acme/skills/tdd", "tdd"),
                 "/skills install did not open the install sheet", journey.tui))
        return;
    journey.tui.send("\x1b");
    journey.tui.pump(500);
    journey.tui.send("q");
    journey.tui.pump(500);

    const std::string pack = journey.root + "/.amber/loose-skill.tar.gz";
    if (!require(run_command(journey.tui, "/set skills install " + pack, "skill installed to"),
                 "the local skill pack was not installed", journey.tui))
        return;
    require(fs::exists(journey.root + "/xdg/amber/skills/loose-skill/SKILL.md"),
            "the installed skill file is missing", journey.tui);
    if (!require(
            run_command(journey.tui, "/skills list", "global \u00b7 authored \u00b7 loose-skill"),
            "/skills list did not show the installed pack", journey.tui))
        return;

    // A locally edited managed skill: the update check names the remote change,
    // and pressing u again is the explicit override. The first pass must leave
    // the edited file alone.
    const std::string local_edit =
        "---\nname: tdd\ndescription: Locally edited.\n---\n\nlocal edit stays\n";
    if (!require_setup(write_text_file(journey.root + "/.amber/skills/tdd/SKILL.md", local_edit),
                       "the local edit was not written"))
        return;
    if (!require(run_command(journey.tui, "/skills update", "tdd"),
                 "/skills update did not open the installed shelf", journey.tui))
        return;
    journey.tui.send("/");
    journey.tui.pump(200);
    journey.tui.send("tdd");
    journey.tui.send("\r");
    journey.tui.pump(300);
    const size_t status_mark = journey.tui.raw.size();
    journey.tui.send("u");
    if (!require(wait_since(journey.tui, status_mark, "local edits"),
                 "the update check did not report the local edit", journey.tui))
        return;
    journey.tui.send("u");
    if (!require(journey.tui.wait_for("press x to override", 15000),
                 "the review did not warn before replacing local edits", journey.tui))
        return;
    journey.tui.send("\x1b");
    journey.tui.pump(500);
    require(read_text(journey.root + "/.amber/skills/tdd/SKILL.md") == local_edit,
            "an unreviewed update replaced the local edit", journey.tui);
    journey.tui.send("q");
    if (!require(journey.tui.wait_for("skills closed", 10000), "q did not close the explorer",
                 journey.tui))
        return;

    if (!require(run_command(journey.tui, "/skills remove loose-skill", "loose-skill"),
                 "/skills remove did not open the installed shelf", journey.tui))
        return;
    journey.tui.send("D");
    if (!require(journey.tui.wait_for("Remove skill", 10000), "D did not open the removal prompt",
                 journey.tui))
        return;
    journey.tui.send("y");
    if (!require(journey.tui.wait_for("removed loose-skill", 10000),
                 "the removal did not report the skill", journey.tui))
        return;
    require(!fs::exists(journey.root + "/xdg/amber/skills/loose-skill"),
            "the removed skill directory is still there", journey.tui);
    journey.finish("skills-manage");
}

void journey_management_surface(const std::string& binary) {
    auto llm = std::make_unique<bench::ScriptedEndpoint>(
        std::deque<bench::EndpointReply>{answer("MANAGEMENT-READY")});
    if (!require_setup(llm->start(), "the management endpoint did not start"))
        return;
    const std::string llm_base = llm->base_url();
    Journey journey;
    if (!require(journey.start_with(binary, "management-surface", std::move(llm), "",
                                    [llm_base](const std::string& root) {
                                        require_setup(prepare_management_fixture(root, llm_base),
                                                      "the management fixtures were not written");
                                    }),
                 "the TUI did not start", journey.tui))
        return;
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;
    // The welcome window is read-only and owns no agent; one turn creates the
    // chat window every management command below is reached through.
    journey.tui.send("Prepare the management session.\r");
    if (!require(journey.tui.wait_for("MANAGEMENT-READY", 30000),
                 "the first turn did not start a chat window", journey.tui))
        return;
    settle_output(journey.tui);

    // Top rail and the amber mark's menu.
    if (!require(journey.tui.wait_for("\u25c6", 5000), "the amber mark is missing", journey.tui))
        return;
    if (mouse_enabled(journey.tui) &&
        !require(mouse_sgr(journey.tui), "the app did not ask for SGR mouse encoding", journey.tui))
        return;
    if (mouse_sgr(journey.tui)) {
        send_click(journey.tui, true, 1, 0);
        if (!require(journey.tui.wait_for("File", 5000), "the mark did not open the menu",
                     journey.tui))
            return;
        journey.tui.send("\x1b");
        journey.tui.pump(500);
    }

    // The completion drawer is the typed path to every documented command.
    journey.tui.raw.clear();
    journey.tui.send("/");
    if (!require(journey.tui.wait_for("Tab complete", 5000), "typing / did not open the drawer",
                 journey.tui))
        return;
    if (!require(journey.tui.wait_for("Show command help", 5000), "the drawer did not list /help",
                 journey.tui))
        return;
    journey.tui.send("\x1b");
    journey.tui.pump(300);
    journey.tui.send("\x15"); // clear the "/" left in the composer
    journey.tui.pump(200);

    if (!require(run_command(journey.tui, "/learn show", "proj-name"),
                 "/learn show did not list the stored memory", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/learn show m1", "content: project uses make"),
                 "/learn show m1 did not open the detail", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/learn pin m1 on", "pinned m1: on"),
                 "/learn pin did not pin the memory", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/learn panel", "Learned items"),
                 "/learn panel did not open", journey.tui))
        return;
    journey.tui.send("\x1b");
    journey.tui.pump(500);
    if (!require(run_command(journey.tui, "/get learn", "memories:"),
                 "the prompt did not return from the learn panel", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/learn forget m1", "removed m1"),
                 "/learn forget did not remove the memory", journey.tui))
        return;

    if (!require(run_command(journey.tui, "/provider list", "custom (http"),
                 "/provider list did not render the active provider", journey.tui))
        return;
    // The catalogue probe is asynchronous: record the request count before the
    // command, because the probe can finish before the "testing ..." line is
    // painted. The screen only has to prove the command started.
    const auto paths_before = journey.endpoint->paths();
    const auto models_before = static_cast<std::size_t>(
        std::count(paths_before.begin(), paths_before.end(), "/v1/models"));
    journey.tui.raw.clear();
    journey.tui.send("/provider test custom\x07");
    if (!require(journey.tui.wait_for("testing custom...", 15000),
                 "/provider test did not start the catalogue probe", journey.tui))
        return;
    bool probed = false;
    for (int waited = 0; waited < 10000 && !probed; waited += 50) {
        journey.tui.pump(50);
        const auto paths = journey.endpoint->paths();
        probed = static_cast<std::size_t>(std::count(paths.begin(), paths.end(), "/v1/models")) >
                 models_before;
    }
    if (!require(probed, "/provider test did not probe the provider's model listing", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/provider delete temp", "deleted provider: temp"),
                 "/provider delete did not remove the temporary provider", journey.tui))
        return;
    require(!fs::exists(agent::global_config_dir() + "/providers/temp.conf"),
            "the deleted provider file is still there", journey.tui);

    if (!require(
            run_command(journey.tui, "/get plugin settings cache metric", "cache metric = tokens"),
            "/get plugin settings did not read the cache metric", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/set plugin settings cache metric calls",
                             "set cache metric = calls"),
                 "/set plugin settings did not write the cache metric", journey.tui))
        return;
    if (!require(
            run_command(journey.tui, "/get plugin settings cache metric", "cache metric = calls"),
            "the plugin setting did not round-trip", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/plugin hello greet Amber", "Hello, Amber!"),
                 "/plugin hello greet did not run the contributed workspace", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/panel", "Plugin registry"),
                 "/panel did not open the registry console", journey.tui))
        return;
    journey.tui.send("\x1b");
    journey.tui.pump(500);

    const std::string pack = journey.root + "/.amber/fake-plugin.tar.gz";
    if (!require(run_command(journey.tui, "/set plugin install " + pack, "activates it"),
                 "the plugin pack was not installed", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/get plugin settings fake", "settings fake: (none)"),
                 "the installed plugin's settings store was not readable", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/set plugin on fake", "plugin fake on"),
                 "the installed plugin did not activate", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/fake echo hi", "unknown command: /fake"),
                 "an external plugin added a command root", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/set plugin off fake", "plugin fake off"),
                 "the installed plugin did not deactivate", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/set plugin uninstall fake", "uninstalled: fake"),
                 "the installed plugin was not removed", journey.tui))
        return;
    require(!fs::exists(agent::global_config_dir() + "/plugins/fake"),
            "the uninstalled plugin directory is still there", journey.tui);

    // Sessions: save the finished turn, rename the stored row, remove it, then
    // fork the live conversation into its own window.
    if (!require(run_command(journey.tui, "/session save", "saved session"),
                 "/session save did not persist the conversation", journey.tui))
        return;
    const std::string session_id = token_after(journey.tui.text(), "saved session ");
    if (!require_setup(!session_id.empty(), "the saved session id was not printed"))
        return;
    const std::string session_file =
        agent::Workspace::state_dir() + "/sessions/" + session_id + ".json";
    if (!require_setup(fs::exists(session_file), "the saved session file is missing"))
        return;
    if (!require(run_command(journey.tui, "/session rename " + session_id + " Renamed Session",
                             "renamed session " + session_id),
                 "/session rename did not update the stored row", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/session remove " + session_id,
                             "deleted session: " + session_id),
                 "/session remove did not delete the stored row", journey.tui))
        return;
    require(!fs::exists(session_file), "the removed session file is still there", journey.tui);
    if (!require(run_command(journey.tui, "/session fork", "forked session into window"),
                 "/session fork did not open the forked window", journey.tui))
        return;

    // /help paints a long transcript; the command after it must still be
    // visible (the live-edge rule this journey pinned in the pty suite).
    if (!require(run_command(journey.tui, "/help", "/undo Restore the"),
                 "/help did not print the command list", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/stop", "stop requested"),
                 "the command after /help was not visible", journey.tui))
        return;
    journey.finish("management-surface");
}

void journey_settings_surface(const std::string& binary) {
    Journey journey;
    if (!require(journey.start(binary, "settings-surface", {answer("SETTINGS-READY")}),
                 "the TUI did not start", journey.tui))
        return;
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;
    journey.tui.send("Prepare the settings session.\r");
    if (!require(journey.tui.wait_for("SETTINGS-READY", 30000),
                 "the first turn did not start a chat window", journey.tui))
        return;
    settle_output(journey.tui);

    if (!require(run_command(journey.tui, "/set", "Use /set <option> <value>"),
                 "the bare /set overview did not print", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/settings", "settings groups"),
                 "the /settings group list did not print", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/get", "Configuration"),
                 "/get did not open the configuration screen", journey.tui))
        return;
    journey.tui.send("\x1b");
    journey.tui.pump(500);
    if (!require(run_command(journey.tui, "/get config", "Configuration"),
                 "/get config did not open the configuration screen", journey.tui))
        return;
    journey.tui.send("\x1b");
    journey.tui.pump(500);
    if (!require(run_command(journey.tui, "/get config paths", "sessions"),
                 "/get config paths did not print the resolved layout", journey.tui))
        return;

    if (!require(run_command(journey.tui, "/get display", "theme: amber"),
                 "the display readout is wrong", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/set display theme modern", "theme: Modern"),
                 "the display theme was not set", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/get display", "theme: modern"),
                 "the display theme did not round-trip", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/set display theme sepia", "usage: /set display theme"),
                 "an invalid theme was accepted", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/get display", "theme: modern"),
                 "an invalid theme changed the value", journey.tui))
        return;

    if (!require(run_command(journey.tui, "/get policy mode", "policy mode: write"),
                 "the policy mode readout is wrong", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/set policy mode read", "policy mode: read"),
                 "the policy mode was not set", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/get policy mode", "policy mode: read"),
                 "the policy mode did not round-trip", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/set policy mode chaos", "usage: /set policy mode"),
                 "an invalid policy mode was accepted", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/get policy approval", "policy approval: on"),
                 "the approval readout is wrong", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/set policy approval off", "policy approval: off"),
                 "the approval switch was not set", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/get policy approval", "policy approval: off"),
                 "the approval switch did not round-trip", journey.tui))
        return;

    if (!require(run_command(journey.tui, "/get work", "work.auto_attach: off"),
                 "the work readout is wrong", journey.tui))
        return;
    if (!require(
            run_command(journey.tui, "/set work auto_attach agents", "work.auto_attach: agents"),
            "the work setting was not applied", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/get work", "work.auto_attach: agents"),
                 "the work setting did not round-trip", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/set work auto_attach sometimes",
                             "work.auto_attach: expected off|agents|jobs|all"),
                 "an invalid work setting was accepted", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/get work", "work.auto_attach: agents"),
                 "an invalid work setting changed the value", journey.tui))
        return;

    if (!require(run_command(journey.tui, "/get worktree", "worktree: on"),
                 "the worktree readout is wrong", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/set worktree off", "worktree: off"),
                 "the worktree setting was not applied", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/get worktree", "worktree: off"),
                 "the worktree setting did not round-trip", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/set worktree maybe", "usage: /set worktree"),
                 "an invalid worktree value was accepted", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/get worktree", "worktree: off"),
                 "an invalid worktree value changed the setting", journey.tui))
        return;

    if (!require(run_command(journey.tui, "/get input", "input.max_lines: 8"),
                 "the input readout is wrong", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/set input max_lines 10", "input.max_lines: 10"),
                 "the input line cap was not applied", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/get input", "input.max_lines: 10"),
                 "the input line cap did not round-trip", journey.tui))
        return;
    if (!require(
            run_command(journey.tui, "/set input max_lines 99", "input.max_lines: expected 1-20"),
            "an invalid input line cap was accepted", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/get input", "input.max_lines: 10"),
                 "an invalid input line cap changed the value", journey.tui))
        return;

    if (!require(run_command(journey.tui, "/get logs", "cap: 256 MB"), "the logs readout is wrong",
                 journey.tui))
        return;
    if (!require(run_command(journey.tui, "/set logs max 128", "logs cap: 128 MB"),
                 "the logs cap was not applied", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/get logs", "cap: 128 MB"),
                 "the logs cap did not round-trip", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/set logs max 0", "logs.max: invalid value"),
                 "an invalid logs cap was accepted", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/get logs", "cap: 128 MB"),
                 "an invalid logs cap changed the value", journey.tui))
        return;

    if (!require(run_command(journey.tui, "/get detection", "detection loop: off"),
                 "the detection readout is wrong", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/set detection loop on", "detection loop: on"),
                 "the loop detector was not enabled", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/get detection", "detection loop: on"),
                 "the loop detector did not round-trip", journey.tui))
        return;
    if (!require(
            run_command(journey.tui, "/set detection loop maybe", "usage: /set detection loop"),
            "an invalid loop value was accepted", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/get detection", "detection loop: on"),
                 "an invalid loop value changed the setting", journey.tui))
        return;

    if (!require(run_command(journey.tui, "/get compression", "compression threshold:"),
                 "the compression readout is wrong", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/set compression threshold 0.7",
                             "compression threshold: 0.700000"),
                 "the compression threshold was not applied", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/get compression", "compression threshold: 0.700000"),
                 "the compression threshold did not round-trip", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/set compression threshold 2",
                             "compression.threshold: invalid value (0.1-1.0)"),
                 "an invalid compression threshold was accepted", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/get compression", "compression threshold: 0.700000"),
                 "an invalid compression threshold changed the value", journey.tui))
        return;

    if (!require(run_command(journey.tui, "/get skills settings", "review on"),
                 "the skills acquisition readout is wrong", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/set skills review off", "skills review: off"),
                 "the skills review gate was not changed", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/get skills settings", "review off"),
                 "the skills review gate did not round-trip", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/set skills review maybe",
                             "usage: /set skills review on|off"),
                 "an invalid skills review value was accepted", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/get skills settings", "review off"),
                 "an invalid skills review value changed the gate", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/set skills create probe-skill",
                             "skill 'probe-skill' created (project)"),
                 "the probe skill was not created", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/set skills delete probe-skill",
                             "skill 'probe-skill' deleted (project)"),
                 "the probe skill was not deleted", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/set skills refresh", "skills refreshed"),
                 "the skill catalog was not refreshed", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/set skills interop on", "skills interop: on"),
                 "the interop scan was not enabled", journey.tui))
        return;

    if (!require(run_command(journey.tui, "/get subagent", "subagent parallel:"),
                 "the subagent readout is wrong", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/set subagent max 2", "subagent max: 2"),
                 "the subagent cap was not applied", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/get subagent", "max: 2"),
                 "the subagent cap did not round-trip", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/set subagent max 0", "usage: /set subagent max <1-16>"),
                 "an invalid subagent cap was accepted", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/get subagent", "max: 2"),
                 "an invalid subagent cap changed the value", journey.tui))
        return;

    if (!require(run_command(journey.tui, "/get reasoning", "configured=off"),
                 "the reasoning readout is wrong", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/set reasoning effort off",
                             "reasoning effort: off (applies from the next turn)"),
                 "the reasoning effort was not set", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/set reasoning effort high",
                             "not available for journey-test"),
                 "an unavailable reasoning effort was accepted", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/get reasoning", "configured=off"),
                 "an unavailable reasoning effort changed the value", journey.tui))
        return;

    if (!require(run_command(journey.tui, "/get model", "model: journey-test (provider: custom)"),
                 "the model readout is wrong", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/set model context 200000",
                             "context window pinned: 200000 tokens"),
                 "the context window was not pinned", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/get model context", "pinned 200000"),
                 "the context window did not round-trip", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/set model context 100", "usage: /set model context"),
                 "an invalid context window was accepted", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/get model context", "pinned 200000"),
                 "an invalid context window changed the value", journey.tui))
        return;

    if (!require(run_command(journey.tui, "/get provider", "flavor: openai"),
                 "the provider readout is wrong", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/set provider cache 1h", "provider cache 1h"),
                 "the provider cache lifetime was not set", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/get provider", "prompt cache 1h"),
                 "the provider cache lifetime did not round-trip", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/set provider cache 2h", "usage: /set provider cache"),
                 "an invalid cache lifetime was accepted", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/get provider", "prompt cache 1h"),
                 "an invalid cache lifetime changed the value", journey.tui))
        return;

    if (!require(run_command(journey.tui, "/get debug", "diagnostics: off"),
                 "the diagnostics readout is wrong", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/set debug on", "diagnostics: on"),
                 "the diagnostics switch was not enabled", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/get debug", "diagnostics: on"),
                 "the diagnostics switch did not round-trip", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/set debug off", "diagnostics: off"),
                 "the diagnostics switch was not disabled", journey.tui))
        return;
    journey.finish("settings-surface");
}

bool prepare_workspace_fixture(const std::string& root) {
    return write_text_file(root + "/alpha.txt", "ALPHA-MARKER\n") &&
           write_text_file(root + "/dir/nested.txt", "NESTED-MARKER\n");
}

void journey_workspace_files(const std::string& binary) {
    Journey journey;
    if (!require(journey.start(binary, "workspace-files", {answer("FILES-SESSION-READY")}, "",
                               [](const std::string& root) {
                                   require_setup(prepare_workspace_fixture(root),
                                                 "the workspace files fixture was not written");
                               }),
                 "the TUI did not start", journey.tui))
        return;
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;
    journey.tui.send("Prepare the workspace session.\r");
    if (!require(journey.tui.wait_for("FILES-SESSION-READY", 30000),
                 "the first turn did not start a chat window", journey.tui))
        return;
    settle_output(journey.tui);

    if (!require(run_command(journey.tui, "/files ls .", "alpha.txt"),
                 "/files ls did not list the workspace", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/files tree .", "nested.txt"),
                 "/files tree did not walk the directory", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/files open alpha.txt", "ALPHA-MARKER"),
                 "/files open did not show the file body", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/files find dir", "nested.txt"),
                 "/files find did not list the directory", journey.tui))
        return;

    if (!require(run_command(journey.tui, "/system mkdir made", "created: made"),
                 "/system mkdir did not create the directory", journey.tui))
        return;
    require(fs::is_directory(journey.root + "/made"), "the created directory is missing",
            journey.tui);
    if (!require(run_command(journey.tui, "/system cp alpha.txt made/copy.txt", "copied"),
                 "/system cp did not copy the file", journey.tui))
        return;
    require(read_text(journey.root + "/made/copy.txt") == "ALPHA-MARKER\n",
            "the copied file body is wrong", journey.tui);
    if (!require(run_command(journey.tui, "/system mv made/copy.txt made/moved.txt", "moved"),
                 "/system mv did not move the file", journey.tui))
        return;
    require(!fs::exists(journey.root + "/made/copy.txt") &&
                fs::exists(journey.root + "/made/moved.txt"),
            "the moved file is in the wrong place", journey.tui);
    if (!require(run_command(journey.tui, "/system info made/moved.txt", "made/moved.txt: file"),
                 "/system info did not describe the file", journey.tui))
        return;
    if (!require(
            run_command(journey.tui, "/system delete made/moved.txt", "deleted: made/moved.txt"),
            "/system delete did not remove the file", journey.tui))
        return;
    require(!fs::exists(journey.root + "/made/moved.txt"), "the deleted file is still there",
            journey.tui);
    if (!require(run_command(journey.tui, "/system rmdir made", "removed dir: made"),
                 "/system rmdir did not remove the empty directory", journey.tui))
        return;
    require(!fs::exists(journey.root + "/made"), "the removed directory is still there",
            journey.tui);

    if (!require(run_command(journey.tui, "/system exec echo WORKSPACE-EXEC", "exec: started"),
                 "/system exec did not start the command", journey.tui))
        return;
    if (!require(journey.tui.wait_for("exec: exit 0", 15000), "/system exec did not finish",
                 journey.tui))
        return;
    require(journey.tui.wait_for("WORKSPACE-EXEC", 5000), "the exec output never appeared",
            journey.tui);

    if (!require(run_command(journey.tui, "/system ps", "ps: started"), "/system ps did not start",
                 journey.tui))
        return;
    require(journey.tui.wait_for("ps: exit 0", 15000), "/system ps did not finish", journey.tui);
    if (!require(run_command(journey.tui, "/system df", "df: started"), "/system df did not start",
                 journey.tui))
        return;
    if (!require(journey.tui.wait_for("df: exit 0", 30000), "/system df did not finish",
                 journey.tui))
        return;
    require(journey.tui.wait_for("Filesystem", 5000), "df printed no filesystem table",
            journey.tui);
    if (!require(run_command(journey.tui, "/system uname", "uname: started"),
                 "/system uname did not start", journey.tui))
        return;
    if (!require(journey.tui.wait_for("uname: exit 0", 15000), "/system uname did not finish",
                 journey.tui))
        return;
    if (!require(run_command(journey.tui, "/system uptime", "uptime: started"),
                 "/system uptime did not start", journey.tui))
        return;
    if (!require(journey.tui.wait_for("uptime: exit 0", 15000), "/system uptime did not finish",
                 journey.tui))
        return;
    require(journey.tui.wait_for("load average", 5000), "uptime printed no load average",
            journey.tui);

    const std::string pidfile = journey.root + "/sleep.pid";
    if (!require_setup(run_shell("sh -c 'sleep 300 & echo $! > " + pidfile + "'"),
                       "the kill fixture process did not start"))
        return;
    const std::string pid = first_token(read_text(pidfile));
    if (!require_setup(!pid.empty(), "the fixture process did not print its pid"))
        return;
    if (!require(run_command(journey.tui, "/system kill " + pid, "killed " + pid),
                 "/system kill did not report the pid", journey.tui))
        return;
    require(::kill(std::atoi(pid.c_str()), 0) != 0, "the killed process is still alive",
            journey.tui);
    journey.finish("workspace-files");
}

bool prepare_worktree_fixture(const std::string& root) {
    return run_git(root, "init -q") && write_text_file(root + "/a.txt", "one\n") &&
           run_git(root, "add a.txt") &&
           run_git(root, "-c user.name=amber-journey -c user.email=journey@example.invalid "
                         "commit -qm init");
}

void journey_worktree_lifecycle(const std::string& binary) {
    Journey journey;
    if (!require(journey.start(binary, "worktree-lifecycle", {answer("WORKTREE-SESSION-READY")}, "",
                               [](const std::string& root) {
                                   require_setup(prepare_worktree_fixture(root),
                                                 "the worktree git fixture was not written");
                               }),
                 "the TUI did not start", journey.tui))
        return;
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;
    journey.tui.send("Prepare the worktree session.\r");
    if (!require(journey.tui.wait_for("WORKTREE-SESSION-READY", 30000),
                 "the first turn did not start a chat window", journey.tui))
        return;
    settle_output(journey.tui);
    const std::string head_before = first_token(git_output(journey.root, "rev-parse HEAD"));
    const std::string branch_before =
        first_token(git_output(journey.root, "rev-parse --abbrev-ref HEAD"));
    const std::string status_before = git_output(journey.root, "status --porcelain");
    const std::string base = agent::worktree_base_dir(journey.root);

    if (!require(run_command(journey.tui, "/worktree new feature-x", "created feature-x"),
                 "/worktree new did not create the workspace", journey.tui))
        return;
    if (!require_setup(fs::exists(base + "/feature-x/a.txt"), "the new worktree has no checkout"))
        return;
    if (!require(run_command(journey.tui, "/worktree list", "feature-x"),
                 "/worktree list did not name the workspace", journey.tui))
        return;
    require(journey.tui.text().find("live") != std::string::npos,
            "/worktree list did not say the workspace is live", journey.tui);
    if (!require(run_command(journey.tui, "/worktree show", "worktree: feature-x (live)"),
                 "/worktree show did not describe the workspace", journey.tui))
        return;
    require(journey.tui.text().find("branch: detached") != std::string::npos,
            "/worktree show did not report the detached branch", journey.tui);
    if (!require(run_command(journey.tui, "/worktree remove feature-x", "is in use"),
                 "a claimed workspace was removable", journey.tui))
        return;

    if (!require(run_command(journey.tui, "/worktree detach", "parked"),
                 "/worktree detach did not park the workspace", journey.tui))
        return;
    require(fs::exists(base + "/feature-x"), "detach deleted the workspace directory", journey.tui);
    if (!require(run_command(journey.tui, "/worktree attach", "available: feature-x"),
                 "/worktree attach did not list the parked workspace", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/worktree attach feature-x", "attached feature-x"),
                 "/worktree attach did not reclaim the workspace", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/worktree detach", "parked"),
                 "/worktree detach did not park the workspace again", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/worktree remove feature-x", "workspace removed"),
                 "a clean parked workspace was not removable", journey.tui))
        return;
    require(!fs::exists(base + "/feature-x"), "the removed workspace directory is still there",
            journey.tui);

    if (!require(run_command(journey.tui, "/worktree new main-x", "created main-x"),
                 "/worktree new did not create the second workspace", journey.tui))
        return;
    journey.tui.raw.clear();
    journey.tui.send("/worktree main\x07");
    if (!require(journey.tui.wait_for("worktree main-x: leave", 10000),
                 "/worktree main did not open the leave prompt", journey.tui))
        return;
    journey.tui.send("j"); // Leave and remove
    journey.tui.pump(200);
    journey.tui.send("\r");
    if (!require(journey.tui.wait_for("left main-x", 15000),
                 "/worktree main did not return to the project root", journey.tui))
        return;
    require(!fs::exists(base + "/main-x"), "main kept the worktree directory", journey.tui);
    if (!require(run_command(journey.tui, "/worktree show", "worktree: none"),
                 "the window did not return to the project root", journey.tui))
        return;

    if (!require(run_command(journey.tui, "/worktree new dirty-x", "created dirty-x"),
                 "/worktree new did not create the dirty workspace", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/worktree detach", "parked"),
                 "/worktree detach did not park the dirty workspace", journey.tui))
        return;
    require_setup(write_text_file(base + "/dirty-x/dirty.txt", "local\n"),
                  "the dirty fixture was not written");
    if (!require(run_command(journey.tui, "/worktree remove dirty-x", "workspace kept"),
                 "a dirty workspace was removed", journey.tui))
        return;
    require_setup(fs::remove(base + "/dirty-x/dirty.txt"), "the dirty fixture was not removed");
    if (!require(run_command(journey.tui, "/worktree remove dirty-x", "workspace removed"),
                 "a cleaned workspace was not removable", journey.tui))
        return;

    if (!require(run_command(journey.tui, "/worktree new stale-x", "created stale-x"),
                 "/worktree new did not create the stale workspace", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/worktree detach", "parked"),
                 "/worktree detach did not park the stale workspace", journey.tui))
        return;
    std::error_code ec;
    fs::remove_all(base + "/stale-x", ec);
    if (!require_setup(!ec, "the stale workspace directory was not removed"))
        return;
    if (!require(run_command(journey.tui, "/worktree prune", "dropped registry entries: stale-x"),
                 "/worktree prune did not drop the vanished workspace", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/worktree list", "none in this project"),
                 "/worktree list did not report an empty registry", journey.tui))
        return;

    const std::string head_after = first_token(git_output(journey.root, "rev-parse HEAD"));
    const std::string branch_after =
        first_token(git_output(journey.root, "rev-parse --abbrev-ref HEAD"));
    const std::string status_after = git_output(journey.root, "status --porcelain");
    require(head_after == head_before && branch_after == branch_before &&
                status_after == status_before,
            "a worktree command moved the user's HEAD, branch or index", journey.tui);
    journey.finish("worktree-lifecycle");
}

void journey_tasks_control(const std::string& binary) {
    int call = 0;
    Journey journey;
    if (!require(journey.start(
                     binary, "tasks-control",
                     [&call](const bench::EndpointRequest& request) {
                         bench::EndpointReply reply;
                         if (request.path != "/v1/chat/completions") {
                             reply.handled = false;
                             return reply;
                         }
                         ++call;
                         if (call == 1)
                             return tool_call("c1", "todowrite",
                                              R"({"todos":[{"id":"1","text":"PLAN-ITEM-ONE",)"
                                              R"("status":"in_progress"},{"id":"2",)"
                                              R"("text":"PLAN-ITEM-TWO","status":"pending"}]})");
                         if (call == 2) {
                             reply.content = request.body.find("PLAN-ITEM-ONE") != std::string::npos
                                                 ? "PLAN-TOOL-DONE"
                                                 : "PLAN-TOOL-MISSING";
                             return reply;
                         }
                         if (call == 3)
                             return tool_call("c2", "process_start",
                                              R"({"command":"sleep 5","label":"JOURNEY-PROCESS"})");
                         if (call == 4) {
                             reply.content =
                                 request.body.find("JOURNEY-PROCESS") != std::string::npos
                                     ? "PROCESS-TOOL-DONE"
                                     : "PROCESS-TOOL-MISSING";
                             return reply;
                         }
                         if (call == 5)
                             return tool_call("c3", "tasks", R"({"action":"list"})");
                         if (call == 6) {
                             reply.content =
                                 request.body.find("JOURNEY-PROCESS") != std::string::npos
                                     ? "TASKS-TOOL-DONE"
                                     : "TASKS-TOOL-MISSING";
                             return reply;
                         }
                         reply.content = "UNEXPECTED-TURN";
                         return reply;
                     }),
                 "the TUI did not start", journey.tui))
        return;
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;

    journey.tui.send("Write the plan. PLAN-TOOL\r");
    if (!require(journey.tui.wait_for("PLAN-TOOL-DONE", 30000),
                 "the plan tool result never reached the model", journey.tui))
        return;
    settle_output(journey.tui);
    journey.tui.send("Start a background process. PROCESS-TOOL\r");
    if (!require(journey.tui.wait_for("Approve action?", 15000),
                 "process_start did not ask for approval", journey.tui))
        return;
    journey.tui.send("1"); // Allow once
    if (!require(journey.tui.wait_for("PROCESS-TOOL-DONE", 30000),
                 "the process_start result never reached the model", journey.tui))
        return;
    settle_output(journey.tui);
    journey.tui.send("List the background tasks. TASKS-TOOL\r");
    if (!require(journey.tui.wait_for("TASKS-TOOL-DONE", 30000),
                 "the tasks tool result never reached the model", journey.tui))
        return;
    settle_output(journey.tui);

    if (!require(run_command(journey.tui, "/tasks list", "JOURNEY-PROCESS"),
                 "/tasks list did not show the tool's task", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/tasks start sleep 30", "started task "),
                 "/tasks start did not create the task", journey.tui))
        return;
    const std::string task = task_id_after(journey.tui.text(), "started task ");
    if (!require_setup(!task.empty(), "the started task id was not shown"))
        return;
    if (!require(run_command(journey.tui, "/tasks show " + task, "state: running"),
                 "/tasks show did not describe the running task", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/tasks wait " + task, "waiting for task"),
                 "/tasks wait did not register", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/tasks output " + task, "id: " + task),
                 "/tasks output did not open the task viewer", journey.tui))
        return;
    journey.tui.send("/window detach\x07");
    journey.tui.pump(500);
    if (!require(run_command(journey.tui, "/window attach task " + task, "id: " + task),
                 "/window attach did not open the task viewer", journey.tui))
        return;
    journey.tui.send("/window detach\x07");
    journey.tui.pump(500);

    journey.tui.raw.clear();
    journey.tui.send("/tasks cancel " + task + "\x07");
    if (!require(journey.tui.wait_for("Cancel task", 10000),
                 "/tasks cancel did not open the confirmation", journey.tui))
        return;
    journey.tui.send("\t"); // switch from No to Yes
    journey.tui.pump(200);
    journey.tui.send("\r");
    if (!require(journey.tui.wait_for("cancel requested for " + task, 10000),
                 "/tasks cancel did not request the cancellation", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/tasks wait " + task, "state: cancelled"),
                 "/tasks wait did not observe the cancelled task", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/tasks cancel " + task, "already cancelled"),
                 "a terminal task reported a second cancel as new work", journey.tui))
        return;

    if (!require(run_command(journey.tui, "/work", "Tasks"), "the Work board did not open",
                 journey.tui))
        return;
    journey.tui.send("\x1b");
    journey.tui.pump(500);
    journey.finish("tasks-control");
}

bool prepare_project_fixture(const std::string& root) {
    return write_text_file(root + "/second/package.json",
                           R"({"name":"journey-second","dependencies":{"react":"18"}})") &&
           write_text_file(root + "/second/src/app.js", "console.log('second');\n");
}

void journey_project_index(const std::string& binary) {
    int call = 0;
    Journey journey;
    if (!require(
            journey.start(
                binary, "project-index",
                [&call](const bench::EndpointRequest& request) {
                    bench::EndpointReply reply;
                    if (request.path != "/v1/chat/completions") {
                        reply.handled = false;
                        return reply;
                    }
                    ++call;
                    if (call == 1) {
                        reply.content = "PROJECT-SESSION-READY";
                        return reply;
                    }
                    if (call == 2)
                        return tool_call(
                            "c1", "index_init",
                            R"({"commit":"# Journey Index\n\n- package.json: the second project\n"})");
                    if (call == 3)
                        return request.body.find("wrote ") != std::string::npos
                                   ? tool_call("c2", "index_show", "{}")
                                   : answer("INDEX-WRITE-MISSING");
                    if (call == 4) {
                        reply.content = request.body.find("Journey Index") != std::string::npos
                                            ? "INDEX-TOOL-DONE"
                                            : "INDEX-READ-MISSING";
                        return reply;
                    }
                    reply.content = "UNEXPECTED-TURN";
                    return reply;
                },
                "",
                [](const std::string& root) {
                    require_setup(prepare_project_fixture(root),
                                  "the second project was not written");
                }),
            "the TUI did not start", journey.tui))
        return;
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;
    journey.tui.send("Prepare the project session.\r");
    if (!require(journey.tui.wait_for("PROJECT-SESSION-READY", 30000),
                 "the first turn did not start a chat window", journey.tui))
        return;
    settle_output(journey.tui);
    const std::string second = journey.root + "/second";
    const std::string original = journey.root;

    if (!require(run_command(journey.tui, "/project show", "project: "),
                 "/project show did not print a root", journey.tui))
        return;
    require(journey.tui.text().find(original) != std::string::npos,
            "/project show did not name the original root", journey.tui);
    if (!require(run_command(journey.tui, "/project open " + second + " --no-worktree",
                             "project opened: "),
                 "/project open did not create the scoped window", journey.tui))
        return;
    require(journey.tui.text().find(second) != std::string::npos,
            "/project open did not name the second root", journey.tui);
    if (!require(run_command(journey.tui, "/project show", "project: "),
                 "/project show did not print a root", journey.tui))
        return;
    require(journey.tui.text().find(second) != std::string::npos,
            "/project show did not follow the new window", journey.tui);
    if (!require(run_command(journey.tui, "/project list", "Open projects"),
                 "/project list did not open", journey.tui))
        return;
    journey.tui.send("\x1b");
    journey.tui.pump(500);

    journey.tui.send("Index this project. INDEX-TOOL\r");
    if (!require(journey.tui.wait_for("Approve action?", 15000),
                 "index_init did not ask to write the index", journey.tui))
        return;
    journey.tui.send("1"); // Allow once
    if (!require(journey.tui.wait_for("INDEX-TOOL-DONE", 30000),
                 "the index tool result never reached the model", journey.tui))
        return;
    settle_output(journey.tui);
    if (!require(run_command(journey.tui, "/plugin project_index index show", "Journey Index"),
                 "the plugin command did not read the written index", journey.tui))
        return;

    journey.tui.raw.clear();
    journey.tui.send("/project close\x07");
    if (!require(journey.tui.wait_for("Close session", 10000),
                 "/project close did not ask to close the window", journey.tui))
        return;
    journey.tui.send("y");
    journey.tui.pump(500);
    if (!require(run_command(journey.tui, "/project show", "project: "),
                 "/project close did not print a root", journey.tui))
        return;
    require(journey.tui.text().find(original) != std::string::npos,
            "/project close did not return to the original window", journey.tui);
    require(fs::exists(second + "/package.json"), "closing the project deleted its directory",
            journey.tui);
    journey.finish("project-index");
}

bool prepare_tools_fixture(const std::string& root) {
    return write_text_file(root + "/notes.txt", "NEEDLE-ALPHA lives here\n") &&
           write_text_file(root + "/other.txt", "nothing to find\n");
}

void journey_workspace_tools(const std::string& binary) {
    auto page =
        std::make_unique<bench::ScriptedEndpoint>([](const bench::EndpointRequest& request) {
            bench::EndpointReply reply;
            if (request.path == "/page") {
                reply.content_type = "text/html";
                reply.body = "<html><body><p>WEB-PAGE-MARKER</p></body></html>";
            } else {
                reply.handled = false;
            }
            return reply;
        });
    if (!require_setup(page->start(), "the loopback page server did not start"))
        return;
    const std::string page_url = page->root_url() + "/page";
    int call = 0;
    Journey journey;
    if (!require(
            journey.start(
                binary, "workspace-tools",
                [&call, page_url](const bench::EndpointRequest& request) {
                    bench::EndpointReply reply;
                    if (request.path != "/v1/chat/completions") {
                        reply.handled = false;
                        return reply;
                    }
                    ++call;
                    if (call == 1)
                        return tool_call("c1", "search",
                                         R"({"pattern":"NEEDLE-ALPHA","mode":"grep"})");
                    if (call == 2) {
                        if (request.body.find("notes.txt") == std::string::npos)
                            return answer("SEARCH-GREP-MISSING");
                        return tool_call("c2", "search",
                                         R"({"pattern":"NEEDLE-ALPHA","mode":"semantic"})");
                    }
                    if (call == 3) {
                        if (request.body.find("notes.txt") == std::string::npos)
                            return answer("SEARCH-SEMANTIC-MISSING");
                        return tool_call("c3", "web_fetch", R"({"url":")" + page_url + R"("})");
                    }
                    if (call == 4)
                        return answer(request.body.find("WEB-PAGE-MARKER") != std::string::npos
                                          ? "TOOLS-DONE"
                                          : "WEB-MISSING");
                    return answer("UNEXPECTED-TURN");
                },
                "",
                [](const std::string& root) {
                    require_setup(prepare_tools_fixture(root), "the tools fixture was not written");
                }),
            "the TUI did not start", journey.tui))
        return;
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;
    journey.tui.send("Search the workspace and fetch the page. TOOLS\r");
    if (!require(journey.tui.wait_for("TOOLS-DONE", 30000),
                 "the search or web tool result never reached the model", journey.tui))
        return;
    require(journey.tui.text().find("SEARCH-") == std::string::npos &&
                journey.tui.text().find("WEB-MISSING") == std::string::npos,
            "a tool result was missing", journey.tui);
    journey.finish("workspace-tools");
}

std::string first_task_id(const std::string& text) {
    const size_t at = text.rfind("task_");
    if (at == std::string::npos)
        return {};
    size_t end = at;
    while (end < text.size()) {
        const char c = text[end];
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'))
            break;
        ++end;
    }
    return text.substr(at, end - at);
}

void journey_watch_surface(const std::string& binary) {
    int call = 0;
    Journey journey;
    if (!require(journey.start(binary, "watch-surface",
                               [&call](const bench::EndpointRequest& request) {
                                   bench::EndpointReply reply;
                                   if (request.path != "/v1/chat/completions") {
                                       reply.handled = false;
                                       return reply;
                                   }
                                   ++call;
                                   if (call == 1)
                                       return tool_call("c1", "watch",
                                                        R"({"action":"start","profile":"process",)"
                                                        R"("command":"echo WATCH-MARKER",)"
                                                        R"("label":"JOURNEY-WATCH","interval_s":1,)"
                                                        R"("deadline_s":3,"observe":"off"})");
                                   if (call == 2) {
                                       reply.content =
                                           request.body.find("task_") != std::string::npos
                                               ? "WATCH-TOOL-DONE"
                                               : "WATCH-TOOL-MISSING";
                                       return reply;
                                   }
                                   reply.content = "UNEXPECTED-TURN";
                                   return reply;
                               }),
                 "the TUI did not start", journey.tui))
        return;
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;
    journey.tui.send("Start a watch on the marker. WATCH-TOOL\r");
    if (!require(journey.tui.wait_for("Approve action?", 15000), "watch did not ask for approval",
                 journey.tui))
        return;
    journey.tui.send("1"); // Allow once
    if (!require(journey.tui.wait_for("WATCH-TOOL-DONE", 30000),
                 "the watch tool result never reached the model", journey.tui))
        return;
    settle_output(journey.tui);

    if (!require(run_command(journey.tui, "/tasks list", "JOURNEY-WATCH"),
                 "/tasks list did not show the watch task", journey.tui))
        return;
    const std::string task = first_task_id(journey.tui.text());
    if (!require_setup(!task.empty(), "the watch task id was not shown"))
        return;
    if (!require(run_command(journey.tui, "/panel watch", "Watches"),
                 "/panel watch did not open the watch panel", journey.tui))
        return;
    require(journey.tui.text().find("process") != std::string::npos,
            "the watch panel did not name the profile", journey.tui);
    journey.tui.send("\x1b");
    journey.tui.pump(500);

    journey.tui.raw.clear();
    journey.tui.send("/tasks wait " + task + "\x07");
    const bool registered = journey.tui.wait_for("waiting for task", 3000);
    if (!registered && !require(journey.tui.wait_for("state: succeeded", 3000),
                                "the watch task never reported a terminal state", journey.tui))
        return;
    if (registered && !require(journey.tui.wait_for("state: succeeded", 15000),
                               "the watch task never reported a terminal state", journey.tui))
        return;
    if (!require(run_command(journey.tui, "/tasks output " + task, "id: " + task),
                 "/tasks output did not open the watch task", journey.tui))
        return;
    require(journey.tui.text().find("WATCH-MARKER") != std::string::npos,
            "the watch output does not contain the observed marker", journey.tui);
    journey.tui.send("/window detach\x07");
    journey.tui.pump(500);
    journey.finish("watch-surface");
}

void journey_coding_library(const std::string& binary) {
    Journey journey;
    if (!require(journey.start(binary, "coding-library",
                               [](const bench::EndpointRequest& request) {
                                   bench::EndpointReply reply;
                                   if (request.path != "/v1/chat/completions") {
                                       reply.handled = false;
                                       return reply;
                                   }
                                   if (request.sequence == 0)
                                       return tool_call("c1", "library_lookup", R"({"list":true})");
                                   reply.content =
                                       request.body.find("architecture-monolith-or-services") !=
                                               std::string::npos
                                           ? "LIBRARY-TOOL-DONE"
                                           : "LIBRARY-RESULT-MISSING";
                                   return reply;
                               }),
                 "the TUI did not start", journey.tui))
        return;
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;
    journey.tui.send("List the coding library. LIBRARY\r");
    if (!require(journey.tui.wait_for("LIBRARY-TOOL-DONE", 30000),
                 "the library tool result never reached the model", journey.tui))
        return;
    journey.finish("coding-library");
}

void journey_mcp_tool_call(const std::string& binary) {
    const std::string fixture = mcp_fixture();
    if (!require_setup(fs::exists(fixture), "the mcp_echo fixture is missing; run make test"))
        return;
    Journey journey;
    if (!require(journey.start(
                     binary, "mcp-tool",
                     [](const bench::EndpointRequest& request) {
                         bench::EndpointReply reply;
                         if (request.path != "/v1/chat/completions") {
                             reply.handled = false;
                             return reply;
                         }
                         if (request.body.find("echo:JOURNEY-MCP") != std::string::npos) {
                             reply.content = "MCP-TOOL-DONE";
                             return reply;
                         }
                         return tool_call("c1", "mcp_echo_echo_tool", R"({"text":"JOURNEY-MCP"})");
                     },
                     "",
                     [&fixture](const std::string& root) {
                         const std::string dir = root + "/.amber/mcp";
                         std::error_code ec;
                         fs::create_directories(dir, ec);
                         require_setup(agent::test::pty::write_file(
                                           dir + "/echo.conf",
                                           "type=stdio\ncommand=" + fixture + "\ncwd=" + root +
                                               "\nenabled=1\nauto_connect=1\ntrusted=1\n"),
                                       "the MCP server config was not written");
                     }),
                 "the TUI did not start", journey.tui))
        return;
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;
    // Auto-connect is asynchronous: its tools exist once the server reports
    // them, so wait for the inventory rather than sleeping.
    journey.tui.send("/mcp list\r");
    if (!require(journey.tui.wait_for("1 tools", 20000), "the MCP server never connected",
                 journey.tui))
        return;
    journey.tui.send("Call the echo tool. JOURNEY-MCP\r");
    if (!require(journey.tui.wait_for("MCP-TOOL-DONE", 30000),
                 "the MCP tool result never reached the model", journey.tui))
        return;
    journey.tui.send("/mcp show echo\r");
    if (!require(journey.tui.wait_for("echo_tool", 8000), "/mcp show did not list the server tool",
                 journey.tui))
        return;
    journey.finish("mcp-tool");
}

// The checkpoint lifecycle is a TUI surface: /session history, diff, undo and
// rollback are slash commands. The journey uses a real git repository so the
// contract "the user's HEAD and branch survive every restore" is observable,
// and it checks the private ref under refs/amber/checkpoints/<session>.
void journey_checkpoint_lifecycle(const std::string& binary) {
    bool prepared = false;
    const auto prepare = [&prepared](const std::string& root) {
        prepared =
            run_git(root, "init -q") &&
            agent::test::pty::write_file(root + "/.gitignore", "xdg/\nstate/\ncache/\nhome/\n") &&
            agent::test::pty::write_file(root + "/checkpoint.txt", "one\n") &&
            run_git(root, "add .gitignore checkpoint.txt") &&
            run_git(root, "-c user.name=amber-journey -c user.email=journey@example.invalid "
                          "commit -q -m initial");
    };
    Journey journey;
    if (!require(journey.start(
                     binary, "checkpoint-lifecycle",
                     {tool_call("c1", "write",
                                R"({"path":"checkpoint.txt","edits":[{"old":"one","new":"two"}]})"),
                      answer("CHECKPOINT-DONE")},
                     "", prepare),
                 "the TUI did not start", journey.tui))
        return;
    if (!require_setup(prepared, "the git fixture was not created"))
        return;
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;

    // Ctrl-G dispatches the line as typed, bypassing the completion drawer;
    // Enter with the drawer open would dispatch the highlighted row instead.
    const auto run = [&journey](const std::string& line) {
        journey.tui.send(line);
        journey.tui.pump(300);
        journey.tui.send("\x07");
        journey.tui.pump(300);
    };

    // Before any file-changing turn there is nothing to undo, and the command
    // says so rather than opening a confirmation with no target.
    run("/session undo");
    if (!require(journey.tui.wait_for("checkpoint undo: no earlier checkpoint", 8000),
                 "undo of nothing did not report the empty history", journey.tui))
        return;

    journey.tui.send("Change checkpoint.txt from one to two. CHECKPOINT-JOURNEY\r");
    if (!require(journey.tui.wait_for("CHECKPOINT-DONE", 30000),
                 "the checkpointed turn did not finish", journey.tui))
        return;
    if (!require(journey.tui.wait_for("checkpoint: turn 1", 10000),
                 "the turn did not report its checkpoint", journey.tui))
        return;
    ASSERT_EQ(read_text(fs::path(journey.root) / "checkpoint.txt"), std::string("two\n"));

    const std::string head_before = read_text(fs::path(journey.root) / ".git/HEAD");
    const std::string commit_before = git_output(journey.root, "rev-parse HEAD");
    ASSERT(!head_before.empty());
    ASSERT(!commit_before.empty());

    run("/session history");
    if (!require(journey.tui.wait_for("checkpoints (newest first", 10000), "history did not open",
                 journey.tui))
        return;
    require(journey.tui.text().find("turn 1") != std::string::npos, "history omitted the turn",
            journey.tui);
    require(journey.tui.text().find("refs/amber/checkpoints/") != std::string::npos,
            "history omitted the private ref", journey.tui);

    run("/session diff");
    if (!require(journey.tui.wait_for("checkpoint diff latest:", 10000), "diff did not open",
                 journey.tui))
        return;
    require(journey.tui.text().find("+two") != std::string::npos, "diff omitted the turn change",
            journey.tui);

    run("/session undo");
    if (!require(journey.tui.wait_for("Restore the working tree to", 5000),
                 "undo confirmation did not open", journey.tui))
        return;
    journey.tui.pump(500);
    journey.tui.send("y");
    if (!require(journey.tui.wait_for("checkpoint undo: restored", 15000), "undo did not finish",
                 journey.tui))
        return;
    ASSERT_EQ(read_text(fs::path(journey.root) / "checkpoint.txt"), std::string("one\n"));

    // The restore is a new checkpoint, so the revert itself is undoable; and
    // the user's HEAD, branch and index were never touched.
    run("/session history");
    if (!require(journey.tui.wait_for("turn 2", 10000),
                 "the revert was not recorded as a new checkpoint", journey.tui))
        return;
    require(journey.tui.text().find("undo to turn") != std::string::npos,
            "the revert checkpoint does not name the undo target", journey.tui);
    ASSERT_EQ(read_text(fs::path(journey.root) / ".git/HEAD"), head_before);
    ASSERT_EQ(git_output(journey.root, "rev-parse HEAD"), commit_before);
    ASSERT_EQ(git_output(journey.root, "status --porcelain"), std::string());

    run("/session rollback 1");
    if (!require(journey.tui.wait_for("Restore the working tree to", 5000),
                 "rollback confirmation did not open", journey.tui))
        return;
    journey.tui.pump(500);
    journey.tui.send("y");
    if (!require(journey.tui.wait_for("checkpoint rollback: restored", 15000),
                 "rollback did not finish", journey.tui))
        return;
    ASSERT_EQ(read_text(fs::path(journey.root) / "checkpoint.txt"), std::string("two\n"));

    run("/session rollback 999");
    if (!require(journey.tui.wait_for("checkpoint rollback: no checkpoint for '999'", 8000),
                 "a missing checkpoint did not report the selector", journey.tui))
        return;

    ASSERT_EQ(read_text(fs::path(journey.root) / ".git/HEAD"), head_before);
    ASSERT_EQ(git_output(journey.root, "rev-parse HEAD"), commit_before);
    journey.finish("checkpoint-lifecycle");
}

void journey_compression_keeps_ui_responsive(const std::string& binary) {
    std::atomic<bool> classifying{false};
    std::atomic<int> classify_calls{0};
    int reads = 0;
    Journey journey;
    if (!require(
            journey.start(
                binary, "compression-responsive",
                [&classifying, &classify_calls, &reads](const bench::EndpointRequest& request) {
                    bench::EndpointReply reply;
                    if (request.path != "/v1/chat/completions") {
                        reply.handled = false;
                        return reply;
                    }
                    std::string last_user;
                    for (const agent::Message& message : request.messages)
                        if (message.role == "user")
                            last_user = message.content;
                    // The extract request replays the classify one as its
                    // cached prefix, so it has to be matched first.
                    if (request.body.find("Review the classification above") != std::string::npos) {
                        reply.content = R"({"memories":[],"skills":[]})";
                        return reply;
                    }
                    if (request.body.find("Analyze the conversation above") != std::string::npos) {
                        ++classify_calls;
                        classifying = true;
                        reply.delay_ms = 2000;
                        reply.content =
                            R"({"summary":"the agent read a.txt four times; it contains hello world","classification":[{"turns":"0-6","tag":"context","summary":"read a.txt"}]})";
                        return reply;
                    }
                    if (last_user.find("AFTER-COMPRESSION-PROMPT") != std::string::npos) {
                        reply.content = "AFTER-COMPRESSION-DONE";
                        return reply;
                    }
                    if (++reads <= 4) {
                        reply =
                            tool_call("c" + std::to_string(reads), "read", R"({"path":"a.txt"})");
                        return reply;
                    }
                    reply.content = "READS-DONE: hello world";
                    return reply;
                },
                // The window is wide enough that the post-compression estimate
                // does not trip the automatic gate: this journey is about the
                // manual /compress worker, and a second pass would hide it.
                "context_size=32000\n"),
            "the TUI did not start", journey.tui))
        return;
    agent::test::pty::write_file(journey.root + "/a.txt", "hello world\n");
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;
    journey.tui.send("Read a.txt four times. JOURNEY-COMPRESS\r");
    if (!require(journey.tui.wait_for("READS-DONE", 30000), "the read turn never finished",
                 journey.tui))
        return;
    // The reply reaches the transcript before the worker releases the window,
    // so a /compress typed in that gap is refused with "agent is busy" and the
    // command is lost. The refusal has no side effect, so retry until the
    // command is accepted rather than racing the turn boundary.
    bool compressing = false;
    for (int waited = 0; waited < 30000 && !compressing; waited += 500) {
        journey.tui.send("/compress\r");
        compressing = journey.tui.wait_for("compressing...", 500);
    }
    if (!require(compressing, "the manual compression never started", journey.tui))
        return;
    bool seen = false;
    for (int waited = 0; waited < 20000 && !seen; waited += 50) {
        seen = classifying.load();
        if (!seen)
            journey.tui.pump(50);
    }
    if (!require(seen, "the compression pass never reached the classifier", journey.tui))
        return;
    journey.tui.send("Ask after the compression. AFTER-COMPRESSION-PROMPT\r");
    if (!require(journey.tui.wait_for("queued (1 in queue)", 8000),
                 "the UI did not accept input while the compression ran", journey.tui))
        return;
    if (!require(journey.tui.wait_for("AFTER-COMPRESSION-DONE", 30000),
                 "the queued prompt did not run after the compression", journey.tui))
        return;
    require(classify_calls == 1, "the compression ran more than once", journey.tui);
    require(journey.endpoint->chat_calls() == 8,
            "expected four reads, one answer, classify, extract and a queued answer", journey.tui);
    journey.finish("compression-responsive");
}

void journey_plugin_tool(const std::string& binary) {
    Journey journey;
    if (!require(journey.start(binary, "plugin-tool",
                               {tool_call("c1", "plugin_fake_echo", R"({"text":"TUI-PLUGIN"})"),
                                answer("TUI-PLUGIN-DONE")},
                               "",
                               [](const std::string& root) {
                                   const std::string dir = root + "/.amber/plugins/fake";
                                   std::error_code ec;
                                   fs::create_directories(dir, ec);
                                   fs::copy_file("tests/plugins/fake_manifest.json",
                                                 dir + "/manifest.json",
                                                 fs::copy_options::overwrite_existing, ec);
                                   fs::copy_file("tests/plugins/fake_plugin.py",
                                                 dir + "/fake_plugin.py",
                                                 fs::copy_options::overwrite_existing, ec);
                                   chmod((dir + "/fake_plugin.py").c_str(), 0755);
                               }),
                 "the TUI did not start", journey.tui))
        return;
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;
    journey.tui.send("/set plugin on fake\r");
    if (!require(journey.tui.wait_for("plugin fake on", 8000),
                 "the external plugin never activated", journey.tui))
        return;
    journey.tui.send("Call the plugin tool. TUI-PLUGIN\r");
    if (!require(journey.tui.wait_for("Approve action?", 15000),
                 "the plugin tool's approval never opened", journey.tui))
        return;
    journey.tui.send("1"); // Allow once
    if (!require(journey.tui.wait_for("TUI-PLUGIN-DONE", 30000),
                 "the plugin tool did not complete the turn", journey.tui))
        return;
    require(journey.tui.text().find("echo:TUI-PLUGIN") != std::string::npos,
            "the plugin's output did not reach the transcript", journey.tui);
    journey.finish("plugin-tool");
}

void journey_provider_dialect(const std::string& binary) {
    Journey journey;
    if (!require(
            journey.start(
                binary, "provider-anthropic",
                [](const bench::EndpointRequest& request) {
                    bench::EndpointReply reply;
                    if (request.path != "/v1/messages") {
                        reply.handled = false; // the endpoint's own /v1/models
                        return reply;
                    }
                    reply.content_type = "application/json";
                    reply.body =
                        R"({"id":"msg_tui_1","type":"message","role":"assistant","model":"journey-test","content":[{"type":"text","text":"TUI-ANTHROPIC-DONE"}],"stop_reason":"end_turn","stop_sequence":null,"usage":{"input_tokens":10,"output_tokens":2}})";
                    return reply;
                },
                "flavor=anthropic\n", {}, true),
            "the TUI did not start", journey.tui))
        return;
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;
    journey.tui.send("Reply done. TUI-ANTHROPIC\r");
    if (!require(journey.tui.wait_for("TUI-ANTHROPIC-DONE", 30000),
                 "the Anthropic dialect answer never arrived", journey.tui))
        return;
    bool saw_messages = false;
    for (const std::string& path : journey.endpoint->paths())
        saw_messages = saw_messages || path == "/v1/messages";
    require(saw_messages, "the client did not use the Messages API path", journey.tui);
    journey.finish("provider-anthropic");
}

// A listing that advertises a capability must reach the wire, and switching
// to a model that advertises nothing must clear it. Unit tests cover the
// parsing; this journey drives the real binary through the catalogue fetch,
// the capability application and all three request shapes.
void journey_capability_ingestion(const std::string& binary) {
    const std::string catalogue = R"({"models":[
      {"name":"models/cap-thinking","inputTokenLimit":131072,"outputTokenLimit":8192,
       "thinking":{"supported":true,"default_enabled":true,"mandatory":false,
                   "supported_levels":["low","high"],"default_level":"high"},
       "verbosity":{"supported_levels":["low","medium","high"],"default_level":"medium"}},
      {"name":"models/cap-plain","inputTokenLimit":32768,"outputTokenLimit":4096}
    ]})";
    Journey journey;
    if (!require(
            journey.start(
                binary, "capability-ingestion",
                [&catalogue](const bench::EndpointRequest& request) {
                    bench::EndpointReply reply;
                    if (request.path == "/v1beta/models") {
                        reply.body = catalogue;
                        return reply;
                    }
                    if (request.path.find(":generateContent") == std::string::npos) {
                        reply.handled = false;
                        return reply;
                    }
                    if (request.sequence == 0)
                        reply.body =
                            R"({"candidates":[{"content":{"role":"model","parts":[)"
                            R"({"text":"CAP-THINKING-ON-DONE"}]},"finishReason":"STOP","index":0}],)"
                            R"("usageMetadata":{"promptTokenCount":10,"candidatesTokenCount":2}})";
                    else if (request.sequence == 1)
                        reply.body =
                            R"({"candidates":[{"content":{"role":"model","parts":[)"
                            R"({"text":"CAP-THINKING-OFF-DONE"}]},"finishReason":"STOP","index":0}],)"
                            R"("usageMetadata":{"promptTokenCount":10,"candidatesTokenCount":2}})";
                    else
                        reply.body =
                            R"({"candidates":[{"content":{"role":"model","parts":[)"
                            R"({"text":"CAP-PLAIN-DONE"}]},"finishReason":"STOP","index":0}],)"
                            R"("usageMetadata":{"promptTokenCount":10,"candidatesTokenCount":2}})";
                    return reply;
                },
                "flavor=gemini\nmodel=cap-thinking\nreasoning_effort=high\napi_key=sk-test\n", {},
                true),
            "the TUI did not start", journey.tui))
        return;

    // Startup reads the disk cache only; the catalogue arrives on a worker.
    bool listed = false;
    for (int waited = 0; waited < 30000 && !listed; waited += 200) {
        journey.tui.pump(200);
        for (const auto& request : journey.endpoint->requests())
            listed = listed || request.path == "/v1beta/models";
    }
    if (!require(listed, "the Gemini catalogue was never fetched", journey.tui))
        return;
    if (!require(journey.tui.wait_for("n_ctx=131072", 20000),
                 "the fetched catalogue was never applied to the session", journey.tui))
        return;

    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;
    journey.tui.send("Reply with the word done. CAP-ON\r");
    if (!require(journey.tui.wait_for("CAP-THINKING-ON-DONE", 30000),
                 "the thinking-on answer never arrived", journey.tui))
        return;

    journey.tui.send("/set reasoning off\r");
    if (!require(journey.tui.wait_for("reasoning effort: off", 10000),
                 "the reasoning effort switch did not land", journey.tui))
        return;
    journey.tui.send("Reply with the word done. CAP-OFF\r");
    if (!require(journey.tui.wait_for("CAP-THINKING-OFF-DONE", 30000),
                 "the thinking-off answer never arrived", journey.tui))
        return;

    journey.tui.send("/model cap-plain\r");
    if (!require(journey.tui.wait_for("model set to cap-plain", 10000),
                 "the model switch did not land", journey.tui))
        return;
    journey.tui.send("Reply with the word done. CAP-PLAIN\r");
    if (!require(journey.tui.wait_for("CAP-PLAIN-DONE", 30000),
                 "the quiet-model answer never arrived", journey.tui))
        return;

    std::vector<agent::json> chat;
    for (const auto& request : journey.endpoint->requests())
        if (request.path.find(":generateContent") != std::string::npos)
            chat.push_back(request.json);
    if (!require(chat.size() == 3u, "expected three Gemini chat requests", journey.tui))
        return;
    const auto config_of = [](const agent::json& body) -> agent::json {
        const auto gen = body.find("generationConfig");
        if (gen == body.end() || !gen->is_object())
            return {};
        const auto cfg = gen->find("thinkingConfig");
        return cfg == gen->end() ? agent::json{} : *cfg;
    };
    const agent::json on = config_of(chat[0]);
    const agent::json off = config_of(chat[1]);
    const agent::json quiet = config_of(chat[2]);
    if (!require(on.value("includeThoughts", false),
                 "the advertised thinking contract did not reach the wire", journey.tui))
        return;
    if (!require(!on.contains("thinkingBudget"), "the journey invented a numeric thinking budget",
                 journey.tui))
        return;
    if (!require(off.value("thinkingBudget", -1) == 0,
                 "thinking off did not use Gemini's documented zero budget", journey.tui))
        return;
    if (!require(!off.contains("includeThoughts"), "thinking off still asked for thoughts",
                 journey.tui))
        return;
    if (!require(quiet.empty(), "the quiet model inherited the previous thinking contract",
                 journey.tui))
        return;
    if (!require(!chat[0].contains("verbosity"),
                 "a listing-derived verbosity was sent without a wire contract", journey.tui))
        return;
    journey.finish("capability-ingestion");
}

// Reasoning tokens are a breakdown of `down`, never a second charge. The unit
// suite pins the parser; these journeys pin the readout and the session round
// trip through the real binary, including the provider that reports no split.
void journey_usage_reasoning_tokens(const std::string& binary) {
    const auto responder = [](bool report_reasoning) {
        return [report_reasoning](const bench::EndpointRequest& request) {
            bench::EndpointReply reply;
            if (request.path != "/v1/chat/completions") {
                reply.handled = false;
                return reply;
            }
            const std::string split =
                report_reasoning ? R"(,"completion_tokens_details":{"reasoning_tokens":8})" : "";
            reply.body =
                std::string(R"({"id":"chatcmpl-usage","object":"chat.completion","created":1,)") +
                R"("model":"journey-test","choices":[{"index":0,"message":{"role":"assistant",)"
                R"("content":"USAGE-REASONING-DONE"},"finish_reason":"stop"}],)"
                R"("usage":{"prompt_tokens":10,"completion_tokens":20,"total_tokens":30)" +
                split + R"(}})";
            return reply;
        };
    };

    // A provider that splits out reasoning tokens: the readout names them and
    // the session meta survives the quit path.
    Journey reported;
    if (!require(reported.start(binary, "usage-reasoning", responder(true)),
                 "the TUI did not start", reported.tui))
        return;
    if (!require(wait_ready(reported.tui, fs::path(reported.root).filename().string()),
                 "the UI never came up", reported.tui))
        return;
    reported.tui.send("Reasoning probe. USAGE-REASONING\r");
    if (!require(reported.tui.wait_for("USAGE-REASONING-DONE", 30000), "the reply did not arrive",
                 reported.tui))
        return;
    reported.tui.send("/usage");
    reported.tui.pump(250);
    reported.tui.send("\x07");
    if (!require(reported.tui.wait_for("thinking 8", 10000),
                 "/usage did not report the reasoning breakdown", reported.tui))
        return;
    if (!require(reported.tui.text().find("down 20") != std::string::npos,
                 "reasoning was added to `down` instead of broken out of it", reported.tui))
        return;
    if (!require(reported.finish("usage-reasoning"), "the TUI did not exit cleanly", reported.tui))
        return;

    bool persisted = false;
    const fs::path state = fs::path(reported.root) / "state";
    std::error_code ec;
    for (fs::recursive_directory_iterator it(state, ec), end; it != end; it.increment(ec)) {
        if (!it->is_regular_file() || it->path().filename() == "workspace.json" ||
            it->path().extension() != ".json")
            continue;
        const agent::json session = agent::json::parse(read_text(it->path()), nullptr, false);
        if (session.is_discarded() || !session.contains("meta"))
            continue;
        const auto& meta = session["meta"];
        if (meta.value("session_usage_main_reasoning_tokens", 0) == 8 &&
            meta.value("session_usage_main_reasoning_measured", false))
            persisted = true;
    }
    require(persisted, "the reasoning total did not survive in the session meta", reported.tui);

    // A provider that reports no split: no breakdown and never a fabricated 0.
    Journey quiet;
    if (!require(quiet.start(binary, "usage-no-reasoning", responder(false)),
                 "the TUI did not start", quiet.tui))
        return;
    if (!require(wait_ready(quiet.tui, fs::path(quiet.root).filename().string()),
                 "the UI never came up", quiet.tui))
        return;
    quiet.tui.send("Reasoning probe. USAGE-REASONING\r");
    if (!require(quiet.tui.wait_for("USAGE-REASONING-DONE", 30000), "the reply did not arrive",
                 quiet.tui))
        return;
    quiet.tui.send("/usage");
    quiet.tui.pump(250);
    quiet.tui.send("\x07");
    if (!require(quiet.tui.wait_for("main: up 10 down 20", 10000),
                 "/usage did not report the measured totals", quiet.tui))
        return;
    require(quiet.tui.text().find("thinking ") == std::string::npos,
            "/usage fabricated a reasoning breakdown the provider never reported", quiet.tui);
    quiet.finish("usage-no-reasoning");
}

// Profiles are connection state, not a readout: a switch must change which
// endpoint receives the next turn, mask its key, wait for a busy turn and be
// pinnable by the project. Both profiles use one loopback endpoint with two
// path prefixes so the request itself says which connection was adopted.
void journey_profile_switch(const std::string& binary) {
    const std::string default_key = "sk-default-0000000000";
    const std::string work_key = "sk-work-1234567890abcd";
    const auto seed_profile = [](const char* name, const char* provider, const char* model,
                                 const std::string& base, const std::string& key) {
        agent::Profile p;
        p.name = name;
        p.provider_name = provider;
        p.flavor = "openai";
        p.api_base = base;
        p.api_key = key;
        p.model = model;
        std::string error;
        return agent::save_profile(p, &error);
    };
    auto chat_seq = std::make_shared<int>(0);
    const auto responder = [chat_seq](const bench::EndpointRequest& request) {
        bench::EndpointReply reply;
        if (request.path.find("/models") != std::string::npos) {
            reply.body = R"({"data":[{"id":"journey-default","context_length":8192},)"
                         R"({"id":"journey-work","context_length":8192}]})";
            return reply;
        }
        const bool work = request.path.find("/work/") != std::string::npos;
        reply.body =
            std::string(R"({"id":"chatcmpl-profile","object":"chat.completion","created":1,)") +
            R"("model":"journey-test","choices":[{"index":0,"message":{"role":"assistant",)"
            R"("content":")" +
            (work ? "WORK-PROFILE-DONE" : "DEFAULT-PROFILE-DONE") +
            R"("},"finish_reason":"stop"}],)"
            R"("usage":{"prompt_tokens":1,"completion_tokens":1,"total_tokens":2}})";
        // The turn that receives a queued switch must still be running when the
        // command arrives; the echo alone is not enough to prove that.
        if ((*chat_seq)++ == 2)
            reply.delay_ms = 1500;
        return reply;
    };

    Journey journey;
    {
        auto server = std::make_unique<bench::ScriptedEndpoint>(responder);
        if (!require(server->start(), "the loopback endpoint did not start", journey.tui))
            return;
        const std::string live = server->base_url();
        const std::string work_base = server->root_url() + "/work/v1";
        if (!require(journey.start_with(binary, "profile-switch", std::move(server),
                                        "model=journey-default\n",
                                        [&](const std::string&) {
                                            seed_profile("default", "custom", "journey-default",
                                                         live, default_key);
                                            seed_profile("work", "work-gateway", "journey-work",
                                                         work_base, work_key);
                                        }),
                     "the TUI did not start", journey.tui))
            return;
    }
    if (!require(wait_ready(journey.tui, fs::path(journey.root).filename().string()),
                 "the UI never came up", journey.tui))
        return;

    journey.tui.send("/profile list\r");
    if (!require(journey.tui.wait_for("work-gateway / journey-work", 10000),
                 "/profile list did not show the work connection", journey.tui))
        return;
    if (!require(journey.tui.wait_for("sk-...abcd", 10000), "/profile list did not mask the key",
                 journey.tui))
        return;

    journey.tui.send("Reply once. DEFAULT-TURN\r");
    if (!require(journey.tui.wait_for("DEFAULT-PROFILE-DONE", 30000),
                 "the first turn did not use the settings connection", journey.tui))
        return;
    journey.tui.send("/profile switch work\r");
    if (!require(journey.tui.wait_for("profile: work (work-gateway, journey-work)", 10000),
                 "the profile switch did not land", journey.tui))
        return;
    journey.tui.send("Reply once. WORK-TURN\r");
    if (!require(journey.tui.wait_for("WORK-PROFILE-DONE", 30000),
                 "the switch did not change the request's endpoint", journey.tui))
        return;

    // A switch asked while the next turn is running must queue, not replace the
    // client that turn is using.
    journey.tui.send("Reply slowly. BUSY-TURN\r");
    if (!require(journey.tui.wait_for("> Reply slowly. BUSY-TURN", 10000),
                 "the busy turn was not submitted", journey.tui))
        return;
    journey.tui.send("/profile switch default\r");
    if (!require(journey.tui.wait_for("profile: switch to default queued", 10000),
                 "the busy switch was not queued", journey.tui))
        return;
    if (!require(journey.tui.wait_for("profile: default (custom, journey-default)", 30000),
                 "the queued switch never landed", journey.tui))
        return;

    journey.tui.send("/profile switch ghost\r");
    if (!require(journey.tui.wait_for("there is no profile named ghost", 10000),
                 "a missing profile did not warn", journey.tui))
        return;
    if (!require(journey.tui.wait_for("profile: default (custom, journey-default)", 10000),
                 "the missing profile changed the connection", journey.tui))
        return;
    if (!require(journey.finish("profile-switch"), "the TUI did not exit cleanly", journey.tui))
        return;

    // Credentials belong to the profile files, never to the workspace or the
    // session meta written under the state dir.
    std::error_code ec;
    for (fs::recursive_directory_iterator it(journey.root, ec), end; it != end; it.increment(ec)) {
        // The XDG config tree legitimately holds keys (profiles + global
        // config); the workspace and the session meta under /state must not.
        if (!it->is_regular_file() || it->path().string().find("/xdg/") != std::string::npos)
            continue;
        const std::string content = read_text(it->path());
        if (!require(content.find(default_key) == std::string::npos &&
                         content.find(work_key) == std::string::npos,
                     "a profile key leaked into " + it->path().string(), journey.tui))
            return;
    }

    // The project pin is a one-line file and must apply before the first turn,
    // so its profile is seeded against the live endpoint before startup.
    Journey pinned;
    {
        auto server = std::make_unique<bench::ScriptedEndpoint>(responder);
        if (!require(server->start(), "the pinned endpoint did not start", pinned.tui))
            return;
        const std::string work_base = server->root_url() + "/work/v1";
        if (!require(pinned.start_with(binary, "profile-project-pin", std::move(server),
                                       "model=journey-default\n",
                                       [&](const std::string& root) {
                                           seed_profile("work", "work-gateway", "journey-work",
                                                        work_base, work_key);
                                           fs::create_directories(fs::path(root) / ".amber", ec);
                                           agent::test::pty::write_file(root + "/.amber/profile",
                                                                        "work\n");
                                       }),
                     "the pinned TUI did not start", pinned.tui))
            return;
    }
    if (!require(wait_ready(pinned.tui, fs::path(pinned.root).filename().string()),
                 "the pinned UI never came up", pinned.tui))
        return;
    pinned.tui.send("Reply once. PIN-TURN\r");
    require(pinned.tui.wait_for("WORK-PROFILE-DONE", 30000),
            "the project pin did not apply before the first turn", pinned.tui);
    pinned.finish("profile-project-pin");
}

// Re-run the journeys in fresh processes. Each journey already owns an
// isolated root, but a fresh process also resets the plugin runtime, the model
// cache and every static the TUI touches.
int repeat_suite(const std::string& self, const std::string& binary, int repeat) {
    for (int iteration = 1; iteration <= repeat; ++iteration) {
        const pid_t child = fork();
        if (child < 0) {
            std::cerr << "FAIL: could not fork for --repeat\n";
            return 1;
        }
        if (child == 0) {
            execlp(self.c_str(), self.c_str(), binary.c_str(), static_cast<char*>(nullptr));
            std::fprintf(stderr, "repeat child: exec(%s): %s\n", self.c_str(),
                         std::strerror(errno));
            _exit(127);
        }
        int status = 0;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
        }
        const bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        std::cout << "--- repeat " << iteration << "/" << repeat << ": "
                  << (ok ? "passed" : "FAILED") << "\n";
        if (!ok)
            return 1;
    }
    return 0;
}

void report_process_failures() {
    for (const std::string& session : agent::test::pty::killed_sessions()) {
        std::cerr << "FAIL: session '" << session
                  << "' was force-killed instead of exiting through the quit path\n";
        failed++;
    }
    for (const std::string& session : agent::test::pty::sessions_without_exit_ready()) {
        std::cerr << "FAIL: session '" << session << "' exited without its exit-ready file\n";
        failed++;
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string binary_arg = "./amber";
    std::string self_arg = argc > 0 ? argv[0] : "tui_agent_e2e_test";
    int repeat = 1;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--repeat" && i + 1 < argc) {
            repeat = std::max(1, std::atoi(argv[++i]));
            continue;
        }
        binary_arg = arg;
    }
    if (repeat > 1) {
        std::error_code ignored;
        const fs::path self = fs::absolute(self_arg, ignored);
        return repeat_suite(ignored ? self_arg : self.string(), binary_arg, repeat);
    }
    const std::string binary = fs::absolute(binary_arg).lexically_normal().string();
    const char* only = std::getenv("AMBER_TUI_JOURNEY");
    if (!only || std::string(only) == "context")
        journey_second_turn_carries_context(binary);
    if (!only || std::string(only) == "approval")
        journey_approval_denied_falls_back(binary);
    if (!only || std::string(only) == "approval-allowed")
        journey_approval_allowed(binary);
    if (!only || std::string(only) == "cancel")
        journey_cancel_slow_tool(binary);
    if (!only || std::string(only) == "tab")
        journey_background_tab_keeps_its_result(binary);
    if (!only || std::string(only) == "switch")
        journey_tab_switch_stays_live(binary);
    if (!only || std::string(only) == "restart")
        journey_restart_resumes_session(binary);
    if (!only || std::string(only) == "checkpoint")
        journey_checkpoint_lifecycle(binary);
    if (!only || std::string(only) == "subagent")
        journey_subagent_reports_back(binary);
    if (!only || std::string(only) == "skill")
        journey_skill_activation(binary);
    if (!only || std::string(only) == "skills-manage")
        journey_skills_management(binary);
    if (!only || std::string(only) == "management-surface")
        journey_management_surface(binary);
    if (!only || std::string(only) == "settings-surface")
        journey_settings_surface(binary);
    if (!only || std::string(only) == "coding-library")
        journey_coding_library(binary);
    if (!only || std::string(only) == "workspace-files")
        journey_workspace_files(binary);
    if (!only || std::string(only) == "worktree-lifecycle")
        journey_worktree_lifecycle(binary);
    if (!only || std::string(only) == "tasks-control")
        journey_tasks_control(binary);
    if (!only || std::string(only) == "project-index")
        journey_project_index(binary);
    if (!only || std::string(only) == "workspace-tools")
        journey_workspace_tools(binary);
    if (!only || std::string(only) == "watch-surface")
        journey_watch_surface(binary);
    if (!only || std::string(only) == "mcp")
        journey_mcp_tool_call(binary);
    if (!only || std::string(only) == "plugin")
        journey_plugin_tool(binary);
    if (!only || std::string(only) == "provider")
        journey_provider_dialect(binary);
    if (!only || std::string(only) == "capability")
        journey_capability_ingestion(binary);
    if (!only || std::string(only) == "usage")
        journey_usage_reasoning_tokens(binary);
    if (!only || std::string(only) == "profile")
        journey_profile_switch(binary);
    if (!only || std::string(only) == "compression")
        journey_compression_keeps_ui_responsive(binary);
    if (!only)
        journey_read_edit_verify(binary);
    report_process_failures();
    if (failed)
        std::cout << "FAILED (" << failed << " failures)\n";
    else
        std::cout << "ALL PASSED\n";
    return failed ? 1 : 0;
}
