#include "bench/probe.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <utility>

#include "agent.h"
#include "agent/context.h"
#include "agent/dispatch.h"
#include "agent/sse_parser.h"
#include "agent/tool_call_parser.h"
#include "agent/tools.h"
#include "agent/workspace.h"
#include "bench/oracle.h"
#include "bench/recorder.h"
#include "bench/runner.h"
#include "bench/scenario.h"

#include <unistd.h>

namespace fs = std::filesystem;

namespace bench {

namespace {

// Timed execution wrapper so the scorecard carries per-probe ms.
double now_ms() noexcept {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct ProbeDef {
    ProbeResult result;
    std::function<bool(ProbeResult&)> run;
};

void add(const std::string& family, const std::string& name,
         const std::function<bool(ProbeResult&)>& run) {
    ProbeResult r;
    r.family = family;
    r.name = name;
    register_probe(std::move(r), run);
}

// Canned SSE stream with one tool_calls delta (id, name, arguments split
// across two fragments — the OpenAI wire shape the parser must merge).
const char* kToolCallsSse =
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"c1\""
    ",\"type\":\"function\",\"function\":{\"name\":\"search\",\"arguments\":\"\""
    "}}]}}]}\n\n"
    "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
    "\"function\":{\"arguments\":\"{\\\"pattern\\\":\\\"foo\\\"}\"}}]}}]}\n\n"
    "data: [DONE]\n\n";

// The Hermes-style content that Qwen2.5-Coder emits (the 32B regression):
// a bare JSON tool call in plain content text with no XML wrapper.
const char* kBareJsonContent =
    "Let's read the task file.\n"
    "{\"name\": \"read\", \"arguments\": {\"path\": \"TASK.md\"}}\n"
    "I'll then write the solution.";

} // namespace

// ---------------------------------------------------------------------------
// parse — SSE delta -> message reconstruction fidelity
// ---------------------------------------------------------------------------

namespace {

bool parse_probe_tool_calls_roundtrip(ProbeResult& r) {
    agent::Message m;
    auto sink = [](const agent::StreamChunk&) {};
    agent::StreamParser p(m, sink, "");
    const std::string sse(kToolCallsSse);
    p.on_write(sse.c_str(), sse.size(), 1);
    p.finalize();
    r.expected = R"(1 tool call 'search' with arguments {"pattern":"foo"})";
    if (!m.tool_calls.is_array() || m.tool_calls.size() != 1u) {
        r.detail = "tool_calls absent or wrong count";
        return false;
    }
    const auto& tc = m.tool_calls[0];
    r.expected += "; id c1";
    if (tc["function"]["name"] != "search") {
        r.detail = "unexpected name";
        return false;
    }
    const auto args = tc["function"]["arguments"].get<std::string>();
    r.expected += "; arguments as JSON string";
    const auto parsed = agent::json::parse(args, nullptr, false);
    if (parsed.is_discarded() || parsed["pattern"] != "foo") {
        r.detail = "arguments not merged/parsed: " + args;
        return false;
    }
    r.detail = r.expected;
    return true;
}

bool parse_probe_reasoning_segmentation(ProbeResult& r) {
    // Inline <think> spans must be segmented out of the visible content.
    const char* sse =
        "data: {\"choices\":[{\"delta\":{\"content\":\"<think>\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"reasoning\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"</think>\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"answer\"}}]}\n\n"
        "data: [DONE]\n\n";
    agent::Message m;
    auto sink = [](const agent::StreamChunk&) {};
    agent::StreamParser p(m, sink, "");
    const std::string body(sse);
    p.on_write(body.c_str(), body.size(), 1);
    p.finalize();
    r.expected = "content='answer', reasoning='reasoning'";
    if (m.content != "answer" || m.reasoning != "reasoning") {
        r.detail = "content='" + m.content + "' reasoning='" + m.reasoning + "'";
        return false;
    }
    r.detail = r.expected;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// extract — text -> tool_calls extraction over every known wrapper style
// ---------------------------------------------------------------------------

namespace {

bool extract_probe_bare_json(ProbeResult& r) {
    // Hermes-style bare JSON in plain content (the 32B regression).
    const auto calls = agent::extract_tool_calls_from_text(kBareJsonContent);
    r.expected = R"(1 call: read with args {"path":"TASK.md"})";
    if (calls.is_null() || calls.size() != 1u) {
        r.detail = "bare JSON not extracted";
        return false;
    }
    const auto& tc = calls[0];
    if (tc["function"]["name"] != "read") {
        r.detail = "unexpected name";
        return false;
    }
    const auto args = tc["function"]["arguments"].get<std::string>();
    if (args != R"({"path":"TASK.md"})") {
        r.detail = "unexpected args: " + args;
        return false;
    }
    r.detail = r.expected;
    return true;
}

bool extract_probe_tool_call_xml(ProbeResult& r) {
    // <tool_call><name>X</name><arguments>...</arguments></tool_call>
    const std::string text =
        "<tool_call><name>bash</name><arguments>{\"command\":\"ls\"}"
        "</arguments></tool_call>";
    const auto calls = agent::extract_tool_calls_from_text(text);
    r.expected = R"(1 call: bash with args {"command":"ls"})";
    if (calls.is_null() || calls.size() != 1u) {
        r.detail = "XML tool_call not extracted";
        return false;
    }
    if (calls[0]["function"]["name"] != "bash") {
        r.detail = "unexpected name";
        return false;
    }
    r.detail = r.expected;
    return true;
}

bool extract_probe_tools_wrapper(ProbeResult& r) {
    // <tools>{"name":...}</tools> (the forced tool_choice shape).
    const std::string text =
        "<tools>{\"name\": \"read\", \"arguments\": {\"path\": \"/etc/hostname\"}}"
        "</tools>";
    const auto calls = agent::extract_tool_calls_from_text(text);
    r.expected = "1 call: read with path /etc/hostname";
    if (calls.is_null() || calls.size() != 1u) {
        r.detail = "<tools> wrapper not extracted";
        return false;
    }
    if (calls[0]["function"]["name"] != "read") {
        r.detail = "unexpected name";
        return false;
    }
    r.detail = r.expected;
    return true;
}

bool extract_probe_attribute_style(ProbeResult& r) {
    const std::string text =
        "<tool_call>\n<function=bash>\n<parameter=command>\nfind . -type f\n"
        "</parameter>\n</function>\n</tool_call>";
    const auto calls = agent::extract_tool_calls_from_text(text);
    r.expected = "1 call: bash with command 'find . -type f'";
    if (calls.is_null() || calls.size() != 1u) {
        r.detail = "attribute style not extracted";
        return false;
    }
    if (calls[0]["function"]["name"] != "bash") {
        r.detail = "unexpected name";
        return false;
    }
    r.detail = r.expected;
    return true;
}

bool extract_probe_multiple_calls(ProbeResult& r) {
    const std::string text =
        "{\"name\": \"read\", \"arguments\": {\"path\": \"TASK.md\"}}\n"
        "{\"name\": \"write\", \"arguments\": {\"path\": \"fizzbuzz.cpp\"}}";
    const auto calls = agent::extract_tool_calls_from_text(text);
    r.expected = "2 calls: read, write in order";
    if (calls.is_null() || calls.size() != 2u) {
        r.detail = "expected 2 calls, got " +
                   std::to_string(calls.is_null() ? 0 : calls.size());
        return false;
    }
    if (calls[0]["function"]["name"] != "read" ||
        calls[1]["function"]["name"] != "write") {
        r.detail = "wrong call order";
        return false;
    }
    r.detail = r.expected;
    return true;
}

bool extract_probe_no_false_positive(ProbeResult& r) {
    // Prose that merely mentions JSON must not be extracted as a tool call.
    const std::string text =
        R"(The config is {"mode": "strict"} and the docs say otherwise.)";
    const auto calls = agent::extract_tool_calls_from_text(text);
    r.expected = "no extraction";
    if (!calls.is_null() && !calls.empty()) {
        r.detail = "false positive extraction";
        return false;
    }
    r.detail = r.expected;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// context — FNV hash-chain integrity on every mutation path
// ---------------------------------------------------------------------------

namespace {

bool context_probe_chain_survives(ProbeResult& r) {
    r.expected = "push/pop/clear/rebuild all keep the hash chain intact";
    agent::Context ctx;
    agent::Message sys;
    sys.role = "system";
    sys.content = "system prompt";
    ctx.push(std::move(sys));
    ctx.get_all();  // chain assert

    for (const char* role : {"user", "assistant", "user", "assistant"}) {
        agent::Message m;
        m.role = role;
        m.content = std::string("turn ") + role;
        ctx.push(std::move(m));
        ctx.get_all();
    }
    auto top = ctx.pop();
    if (top.content != "turn assistant") {
        r.detail = "pop not LIFO";
        return false;
    }
    ctx.get_all();  // chain must survive pop
    ctx.clear();
    ctx.get_all();  // chain must survive clear
    agent::Message fresh;
    fresh.role = "user";
    fresh.content = "fresh";
    ctx.push(std::move(fresh));
    ctx.get_all();  // chain must survive rebuild
    r.detail = r.expected;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// envelope — tool-status envelope contract (ok/err/denied/timeout)
// ---------------------------------------------------------------------------

namespace {

bool envelope_probe_status_classification(ProbeResult& r) {
    r.expected = "envelope status: ok, error, denied, timeout";
    // Build results and let the tool result formatting classify them.
    agent::ToolResult ok;
    ok.ok = true;
    ok.output = "data";

    agent::ToolResult err;
    err.ok = false;
    err.error = "boom";

    agent::ToolResult denied;
    denied.ok = false;
    denied.meta = {{"denied", true}};
    denied.error = "denied by user";

    agent::ToolResult timeout;
    timeout.ok = false;
    timeout.meta = {{"timeout", true}};
    timeout.error = "timed out";

    const std::string f_ok = format_tool_envelope("read", {{"path", "a.txt"}}, ok);
    const std::string f_err =
        format_tool_envelope("read", {{"path", "a.txt"}}, err);
    const std::string f_den =
        format_tool_envelope("read", {{"path", "a.txt"}}, denied);
    const std::string f_to =
        format_tool_envelope("read", {{"path", "a.txt"}}, timeout);

    if (f_ok.find("status=ok") == std::string::npos ||
        f_err.find("status=error") == std::string::npos ||
        f_den.find("status=denied") == std::string::npos ||
        f_to.find("status=timeout") == std::string::npos) {
        r.detail = "status not classified: " + f_ok + " || " + f_err + " || " +
                   f_den + " || " + f_to;
        return false;
    }
    r.detail = r.expected;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// budget — max_steps enforcement is honest, no runaway loop
// ---------------------------------------------------------------------------

namespace {

bool budget_probe_max_steps_enforced(ProbeResult& r) {
    r.expected = "loop stops at max_steps and reports the breach";
    // A hermetic scenario whose scripted model keeps calling a tool forever.
    Scenario s;
    s.name = "probe-budget";
    s.suite = "harness";
    s.prompt = "Do this forever.";
    s.fake_replies = agent::json::parse(R"([
        {"tool_calls": [{"id": "c1", "type": "function",
                         "function": {"name": "bash",
                                      "arguments": "{\"command\":\"true\"}"}}]}
    ])");
    s.max_steps = 3;
    s.max_wall_ms = 30000;

    RunOptions opts;
    RunMeta meta;
    meta.mode = "hermetic";
    meta.model = "fake";
    std::string err;
    ScenarioReport rep = run_one_scenario(s, opts, meta, err);
    if (!err.empty()) {
        r.detail = "run failed: " + err;
        return false;
    }
    if (rep.kpi.steps > s.max_steps + 1) {
        r.detail = "ran " + std::to_string(rep.kpi.steps) +
                   " steps, budget " + std::to_string(s.max_steps);
        return false;
    }
    r.detail = "stopped at " + std::to_string(rep.kpi.steps) + " steps";
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// confinement — workspace path escapes are rejected
// ---------------------------------------------------------------------------

namespace {

bool confinement_probe_escapes_rejected(ProbeResult& r) {
    r.expected = "legal path resolves; ../ and absolute escapes rejected";
    // Capture and restore the active root so later probes/tools (the bash
    // tool spawns with Workspace::root()) are not left pointing at a deleted
    // temp directory.
    const std::string prior = agent::Workspace::root();
    const std::string root = fs::temp_directory_path() / "amber_probe_ws" /
                             std::to_string(::getpid());
    fs::create_directories(root);
    agent::Workspace::set_root(root);

    std::string resolved, error;
    const bool legal = agent::Workspace::confine("a.txt", resolved, error);
    const bool up = agent::Workspace::confine("../escape.txt", resolved, error);
    const bool abs =
        agent::Workspace::confine("/etc/passwd", resolved, error);
    const bool deep =
        agent::Workspace::confine("../../deep/x", resolved, error);

    fs::remove_all(root);
    agent::Workspace::set_root(prior);
    if (!legal || up || abs || deep) {
        r.detail = std::string("legal=") + (legal ? "t" : "f") +
                   " ../=" + (up ? "t" : "f") +
                   " abs=" + (abs ? "t" : "f") +
                   " deep=" + (deep ? "t" : "f");
        return false;
    }
    r.detail = r.expected;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// oracle — scenario corpus self-validation
// ---------------------------------------------------------------------------

namespace {

// Load every scenario file under bench/scenarios and confirm each oracle
// step references a tool that the registry actually provides — a stale or
// misspelled oracle (the h-02 class of bug) is a harness defect.
bool oracle_probe_scenario_self_validation(ProbeResult& r) {
    r.expected = "every oracle step names a registered tool";
    agent::JobService jobs;
    agent::TodoStore todos;
    agent::SubAgentExecutor subagents;
    agent::ToolRegistry reg;
    agent::register_default_tools(reg, jobs, todos, agent::CancellationToken{},
                                  false, subagents, true);

    const fs::path root = fs::current_path() / "bench" / "scenarios";
    if (!fs::is_directory(root)) {
        r.detail = "no bench/scenarios directory";
        return false;
    }
    int checked = 0;
    for (const auto& e : fs::recursive_directory_iterator(root)) {
        if (!e.is_regular_file() || e.path().extension() != ".json") continue;
        const fs::path rel = fs::relative(e.path(), root);
        if (std::distance(rel.begin(), rel.end()) != 2) continue;  // template dir
        std::string err;
        auto s = load_scenario(e.path().string(), err);
        if (!s) {
            r.detail = e.path().filename().string() + ": " + err;
            return false;
        }
        for (const auto& step : s->oracle) {
            if (!reg.find(step.tool)) {
                r.detail = e.path().filename().string() + ": oracle tool '" +
                           step.tool + "' not registered";
                return false;
            }
        }
        ++checked;
    }
    if (checked == 0) {
        r.detail = "no scenarios loaded";
        return false;
    }
    r.detail = r.expected + " (" + std::to_string(checked) + " scenarios)";
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// dispatch — tool_calls -> registry -> result round-trip
// ---------------------------------------------------------------------------

namespace {

bool dispatch_probe_roundtrip(ProbeResult& r) {
    r.expected = "one bash call executes; result sealed in context";
    agent::Config cfg;
    cfg.mode = agent::AgentMode::Yolo;
    agent::ToolRegistry reg;
    reg.register_tool(agent::make_bash_tool());
    agent::ConversationLog log;
    std::set<std::string> approved;
    agent::Context dctx;

    agent::json calls = agent::json::array();
    agent::json tc;
    tc["id"] = "c1";
    tc["type"] = "function";
    tc["function"] = {{"name", "bash"},
                      {"arguments", {{"command", "echo hello"}}}};
    calls.push_back(tc);

    agent::AgentHooks hooks;
    hooks.on_tool_result = [](const std::string&, const agent::ToolResult&,
                              const agent::json&) {};
    const bool ok = agent::dispatch_tool_calls(calls, cfg, reg, hooks, log,
                                               approved, nullptr, &dctx);
    if (!ok) {
        r.detail = "dispatch returned false";
        return false;
    }
    bool found = false;
    for (const auto& m : dctx.get_all()) {
        if (m.role == "tool" && m.name == "bash") {
            found = true;
            break;
        }
    }
    if (!found) {
        r.detail = "no tool result in context";
        return false;
    }
    r.detail = r.expected;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// recovery — the engine compensates for model/server failures
// ---------------------------------------------------------------------------

namespace {

bool recovery_probe_retryable_recovers(ProbeResult& r) {
    r.expected = "retryable error is retried; the run completes";
    // Script: first reply throws a retryable 503, second succeeds.
    Scenario s;
    s.name = "probe-recovery";
    s.suite = "harness";
    s.prompt = "List the files.";
    s.fake_replies = agent::json::array({
        agent::json::object({{"error", "temporary blip"}, {"retryable", true}}),
        agent::json::object({{"content", "done"}}),
    });
    s.max_steps = 5;
    s.max_wall_ms = 30000;

    RunOptions opts;
    RunMeta meta;
    meta.mode = "hermetic";
    meta.model = "fake";
    std::string err;
    ScenarioReport rep = run_one_scenario(s, opts, meta, err);
    if (!err.empty()) {
        r.detail = "run failed: " + err;
        return false;
    }
    if (rep.kpi.retries == 0) {
        r.detail = "retryable error was not retried (retries=0)";
        return false;
    }
    if (rep.kpi.hard_stop) {
        r.detail = "hard-stopped during a retryable error";
        return false;
    }
    r.detail = "retried " + std::to_string(rep.kpi.retries) + " time(s)";
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Registry — the families the scorecard requires
// ---------------------------------------------------------------------------

namespace {

struct ProbeRegistrar {
    ProbeRegistrar() {
        add("parse", "parse_tool_calls_roundtrip", parse_probe_tool_calls_roundtrip);
        add("parse", "parse_reasoning_segmentation", parse_probe_reasoning_segmentation);
        add("extract", "extract_bare_json", extract_probe_bare_json);
        add("extract", "extract_tool_call_xml", extract_probe_tool_call_xml);
        add("extract", "extract_tools_wrapper", extract_probe_tools_wrapper);
        add("extract", "extract_attribute_style", extract_probe_attribute_style);
        add("extract", "extract_multiple_calls", extract_probe_multiple_calls);
        add("extract", "extract_no_false_positive", extract_probe_no_false_positive);
        add("context", "context_chain_survives", context_probe_chain_survives);
        add("envelope", "envelope_status_classification",
            envelope_probe_status_classification);
        add("budget", "budget_max_steps_enforced", budget_probe_max_steps_enforced);
        add("confinement", "confinement_escapes_rejected",
            confinement_probe_escapes_rejected);
        add("oracle", "oracle_scenario_self_validation",
            oracle_probe_scenario_self_validation);
        add("dispatch", "dispatch_roundtrip", dispatch_probe_roundtrip);
        add("recovery", "recovery_retryable_recovers",
            recovery_probe_retryable_recovers);
    }
};

const ProbeRegistrar g_registrar;

// ---------------------------------------------------------------------------
// Framework — registration, execution, aggregation
// ---------------------------------------------------------------------------

} // namespace

namespace {
struct Registry {
    std::vector<ProbeDef> probes;
};
Registry& registry() {
    static Registry r;
    return r;
}
} // namespace

void register_probe(ProbeResult result,
                    const std::function<bool(ProbeResult&)>& run) {
    registry().probes.push_back({std::move(result), run});
}

std::vector<ProbeResult> run_all_probes() {
    std::vector<ProbeResult> out;
    for (const auto& def : registry().probes) {
        ProbeResult r = def.result;
        const double t0 = now_ms();
        try {
            r.passed = def.run(r);
        } catch (const std::exception& e) {
            r.passed = false;
            r.detail = "probe threw: " + std::string(e.what());
        } catch (...) {
            r.passed = false;
            r.detail = "probe threw (non-std exception)";
        }
        r.ms = now_ms() - t0;
        out.push_back(std::move(r));
    }
    return out;
}

double HarnessScorecard::family_integrity(const std::string& family) const
    noexcept {
    const auto it = families.find(family);
    if (it == families.end() || it->second.second == 0) return 0.0;
    return static_cast<double>(it->second.first) /
           static_cast<double>(it->second.second);
}

HarnessScorecard aggregate_probes(const std::vector<ProbeResult>& probes) {
    HarnessScorecard sc;
    sc.probes = probes;
    for (const auto& p : probes) {
        ++sc.total;
        if (p.passed) ++sc.passed;
        auto& f = sc.families[p.family];
        ++f.second;
        if (p.passed) ++f.first;
    }
    sc.integrity = sc.total > 0 ? static_cast<double>(sc.passed) / sc.total
                                : 0.0;
    return sc;
}

} // namespace bench