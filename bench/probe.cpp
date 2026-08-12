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

// P-context-compression: the compression rebuild path (clear + push of the
// returned message list) must preserve message content and the hash chain —
// the Memento contract compress_now relies on.
bool context_probe_compression_rebuild(ProbeResult& r) {
    r.expected = "clear + push rebuild preserves content and chain";
    agent::Context ctx;
    agent::Message sys;
    sys.role = "system";
    sys.content = "system prompt";
    ctx.push(std::move(sys));

    agent::Message u1, a1, u2;
    u1.role = "user";
    u1.content = "hello";
    a1.role = "assistant";
    a1.content = "hi there";
    u2.role = "user";
    u2.content = "what is c++";
    ctx.push(std::move(u1));
    ctx.push(std::move(a1));
    ctx.push(std::move(u2));

    // Simulate the compression output: a rebuilt (possibly reduced) list.
    auto rebuilt = ctx.get_all();
    ctx.clear();
    for (auto& m : rebuilt) ctx.push(std::move(m));
    ctx.get_all();  // chain must survive the rebuild

    const auto& final = ctx.get_all();
    if (final.size() != 4u) {
        r.detail = "rebuild lost messages: " + std::to_string(final.size());
        return false;
    }
    if (final.back().content != "what is c++") {
        r.detail = "rebuild corrupted content: " + final.back().content;
        return false;
    }
    r.detail = "4 messages survived clear+push with chain intact";
    return true;
}

// P-context-tokens: token_count() must stay consistent through push/pop/
// clear/rebuild (budget decisions depend on it).
bool context_probe_token_fidelity(ProbeResult& r) {
    r.expected = "token count matches content through every mutation";
    agent::Context ctx;
    agent::Message sys;
    sys.role = "system";
    sys.content = "system prompt";
    ctx.push(std::move(sys));
    const size_t t0 = ctx.token_count();
    if (t0 == 0) {
        r.detail = "system message contributed 0 tokens";
        return false;
    }

    agent::Message u;
    u.role = "user";
    u.content = "hello world";
    ctx.push(std::move(u));
    const size_t t1 = ctx.token_count();
    auto popped = ctx.pop();
    if (ctx.token_count() != t0) {
        r.detail = "pop did not restore the token count";
        return false;
    }
    if (popped.content != "hello world") {
        r.detail = "pop returned the wrong message";
        return false;
    }
    ctx.clear();
    if (ctx.token_count() != 0) {
        r.detail = "clear left tokens behind";
        return false;
    }
    agent::Message m;
    m.role = "user";
    m.content = "x";
    ctx.push(std::move(m));
    if (ctx.token_count() == 0 || t1 == 0) {
        r.detail = "token accounting broken";
        return false;
    }
    r.detail = "tokens consistent through push/pop/clear/rebuild";
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
// loop — the agentic loop: termination signals, runaway breakout, steering
// ---------------------------------------------------------------------------

namespace {

// A scripted tool call reply (single bash call).
agent::json scripted_bash(const char* id, const char* command) {
    (void)id;
    agent::json tc;
    tc["id"] = "c1";
    tc["type"] = "function";
    tc["function"] = {{"name", "bash"},
                      {"arguments",
                       agent::json::object({{"command", command}})}};
    return agent::json::object({{"tool_calls", agent::json::array({tc})}});
}

// Run a scripted scenario and return the report (or empty failure).
bool run_loop_scenario(Scenario s, ScenarioReport& rep, std::string& err,
                       ProbeResult& r) {
    s.suite = "harness";
    s.max_wall_ms = 60000;
    RunOptions opts;
    RunMeta meta;
    meta.mode = "hermetic";
    meta.model = "fake";
    rep = run_one_scenario(s, opts, meta, err);
    if (!err.empty()) {
        r.detail = "run failed: " + err;
        return false;
    }
    return true;
}

// P-loop-done-flag: the model signals "done" after tool work; the loop must
// terminate on the done flag with no extra dispatch and no recovery noise.
bool loop_probe_done_flag(ProbeResult& r) {
    r.expected = "loop terminates on 'done'; no extra dispatch, no steer";
    Scenario s;
    s.name = "probe-loop-done";
    s.prompt = "Do one thing and finish.";
    s.max_steps = 6;
    // One tool call, then a plain "done" content reply. try_confirm makes a
    // confirmation probe call, so the script carries a second "done".
    s.fake_replies = agent::json::parse(R"([
        {"tool_calls": [{"id": "c1", "type": "function",
                         "function": {"name": "bash",
                                      "arguments": "{\"command\":\"echo hi\"}"}}]},
        {"content": "done"},
        {"content": "done"}
    ])");
    ScenarioReport rep;
    std::string err;
    if (!run_loop_scenario(s, rep, err, r)) return false;
    if (rep.kpi.hard_stop) {
        r.detail = "hard_stop despite a done signal";
        return false;
    }
    if (!rep.kpi.recoveries == 0) {
        r.detail = "unexpected recovery/steer on a clean done";
        return false;
    }
    if (rep.kpi.tool_calls != 1) {
        r.detail = "expected exactly 1 executed tool call, got " +
                   std::to_string(rep.kpi.tool_calls);
        return false;
    }
    r.detail = "terminated after " + std::to_string(rep.kpi.steps) +
               " steps; 1 tool call";
    return true;
}

// P-loop-continue-flag: while the model keeps issuing tool calls, the loop
// must NOT terminate early — it continues until the model stops.
bool loop_probe_continue_flag(ProbeResult& r) {
    r.expected = "loop continues across multiple tool-call turns";
    Scenario s;
    s.name = "probe-loop-continue";
    s.prompt = "Keep going through several steps.";
    s.max_steps = 12;
    s.fake_replies = agent::json::parse(R"([
        {"tool_calls": [{"id": "c1", "type": "function",
                         "function": {"name": "bash",
                                      "arguments": "{\"command\":\"echo 1\"}"}}]},
        {"tool_calls": [{"id": "c2", "type": "function",
                         "function": {"name": "bash",
                                      "arguments": "{\"command\":\"echo 2\"}"}}]},
        {"tool_calls": [{"id": "c3", "type": "function",
                         "function": {"name": "bash",
                                      "arguments": "{\"command\":\"echo 3\"}"}}]},
        {"content": "done"},
        {"content": "done"}
    ])");
    ScenarioReport rep;
    std::string err;
    if (!run_loop_scenario(s, rep, err, r)) return false;
    if (rep.kpi.tool_calls != 3) {
        r.detail = "expected 3 tool calls, got " +
                   std::to_string(rep.kpi.tool_calls);
        return false;
    }
    if (rep.kpi.hard_stop || !rep.kpi.recoveries == 0) {
        r.detail = "loop wrongly terminated early";
        return false;
    }
    r.detail = "ran " + std::to_string(rep.kpi.steps) +
               " steps across 3 tool turns";
    return true;
}

// P-loop-infinite-breakout: an identical tool call repeated forever must be
// broken by loop detection (same fingerprint 3x -> "breaking tool loop").
bool loop_probe_infinite_breakout(ProbeResult& r) {
    r.expected = "identical tool call repeated 3x breaks the loop";
    Scenario s;
    s.name = "probe-loop-breakout";
    s.prompt = "Repeat this call forever.";
    s.max_steps = 12;
    s.detection_loop = true;
    // Script 6 identical calls; detection must break at the 3rd repeat.
    s.fake_replies = agent::json::array();
    for (int i = 0; i < 6; ++i)
        s.fake_replies.push_back(scripted_bash("c1", "echo x"));
    ScenarioReport rep;
    std::string err;
    if (!run_loop_scenario(s, rep, err, r)) return false;
    if (rep.kpi.steps >= 6) {
        r.detail = "loop not broken: ran " + std::to_string(rep.kpi.steps) +
                   " steps of an identical call";
        return false;
    }
    if (rep.kpi.recoveries == 0) {
        r.detail = "no recovery/steer recorded for the breakout";
        return false;
    }
    const std::string text = rep.final_text;
    if (text.find("loop detected") == std::string::npos &&
        text.find("repeated the same tool call") == std::string::npos) {
        r.detail = "final reply does not name the loop: " + text;
        return false;
    }
    r.detail = "broken at step " + std::to_string(rep.kpi.steps);
    return true;
}

// P-loop-text-repeat: identical plain text repeated must get a steer at 2x
// and a hard stop at 5x.
bool loop_probe_text_repeat(ProbeResult& r) {
    r.expected = "same text twice steers; loop stays contained";
    Scenario s;
    s.name = "probe-loop-text";
    s.prompt = "Repeat the same sentence.";
    s.max_steps = 12;
    s.detection_loop = true;
    // A no-tool reply triggers the confirmation exchange; the probe call
    // must return tool calls (so confirm does not accept and end the turn),
    // letting the main text repeat until the loop detector steers (2x).
    s.fake_replies = agent::json::array();
    for (int i = 0; i < 6; ++i) {
        s.fake_replies.push_back(agent::json::object({{"content", "hello?"}}));
        s.fake_replies.push_back(scripted_bash("c1", "echo x"));
    }
    ScenarioReport rep;
    std::string err;
    if (!run_loop_scenario(s, rep, err, r)) return false;
    if (rep.kpi.recoveries == 0) {
        r.detail = "no steer recorded for repeated text";
        return false;
    }
    if (rep.kpi.hard_stop) {
        r.detail = "unexpected hard stop";
        return false;
    }
    r.detail = "steered after " + std::to_string(rep.kpi.steps) +
               " steps; loop contained";
    return true;
}

// P-loop-fail-streak: a failing call repeated 3x must trigger the recovery
// steer; a further failing call then hard-stops (fail streak escalation).
bool loop_probe_fail_streak(ProbeResult& r) {
    r.expected = "failing calls steer once, then hard-stop";
    Scenario s;
    s.name = "probe-loop-failstreak";
    s.prompt = "Keep failing.";
    s.max_steps = 12;
    s.detection_loop = true;
    // A bash call that always fails (exit 1). FailStreak counts 3 repeats.
    s.fake_replies = agent::json::parse(R"([
        {"tool_calls": [{"id": "c1", "type": "function",
                         "function": {"name": "bash",
                                      "arguments": "{\"command\":\"exit 1\"}"}}]},
        {"tool_calls": [{"id": "c2", "type": "function",
                         "function": {"name": "bash",
                                      "arguments": "{\"command\":\"exit 1\"}"}}]},
        {"tool_calls": [{"id": "c3", "type": "function",
                         "function": {"name": "bash",
                                      "arguments": "{\"command\":\"exit 1\"}"}}]},
        {"tool_calls": [{"id": "c4", "type": "function",
                         "function": {"name": "bash",
                                      "arguments": "{\"command\":\"exit 1\"}"}}]},
        {"tool_calls": [{"id": "c5", "type": "function",
                         "function": {"name": "bash",
                                      "arguments": "{\"command\":\"exit 1\"}"}}]}
    ])");
    ScenarioReport rep;
    std::string err;
    if (!run_loop_scenario(s, rep, err, r)) return false;
    if (rep.kpi.recoveries == 0) {
        r.detail = "no recovery steer recorded for failing calls";
        return false;
    }
    if (rep.kpi.tool_failures < 3) {
        r.detail = "expected >=3 tool failures, got " +
                   std::to_string(rep.kpi.tool_failures);
        return false;
    }
    r.detail = "steered after " + std::to_string(rep.kpi.tool_failures) +
               " failures";
    return true;
}

// P-loop-no-false-positive: distinct tool calls (same tool, different args)
// must NOT trip loop detection — the detector only fires on identical calls.
bool loop_probe_no_false_positive(ProbeResult& r) {
    r.expected = "distinct calls never break the loop";
    Scenario s;
    s.name = "probe-loop-nofalse";
    s.prompt = "Do several different things.";
    s.max_steps = 12;
    s.detection_loop = true;
    s.fake_replies = agent::json::array();
    for (int i = 0; i < 4; ++i) {
        const std::string cmd = "echo " + std::to_string(i);
        s.fake_replies.push_back(scripted_bash("c1", cmd.c_str()));
    }
    s.fake_replies.push_back(agent::json::object({{"content", "done"}}));
    s.fake_replies.push_back(agent::json::object({{"content", "done"}}));
    ScenarioReport rep;
    std::string err;
    if (!run_loop_scenario(s, rep, err, r)) return false;
    if (!rep.kpi.recoveries == 0 || rep.kpi.hard_stop) {
        r.detail = "distinct calls wrongly flagged as a loop";
        return false;
    }
    if (rep.kpi.tool_calls != 4) {
        r.detail = "expected 4 tool calls, got " +
                   std::to_string(rep.kpi.tool_calls);
        return false;
    }
    r.detail = "4 distinct calls ran without flagging";
    return true;
}

// P-loop-hard-stop-honesty: a runaway (infinite tool loop with detection
// disabled) must hit the step budget and report hard_stop / failure.
bool loop_probe_hard_stop_honesty(ProbeResult& r) {
    r.expected = "runaway without detection stops at budget, honestly";
    Scenario s;
    s.name = "probe-loop-hardstop";
    s.prompt = "Loop forever.";
    s.max_steps = 3;
    s.detection_loop = false;
    s.fake_replies = agent::json::parse(R"([
        {"tool_calls": [{"id": "c1", "type": "function",
                         "function": {"name": "bash",
                                      "arguments": "{\"command\":\"true\"}"}}]}
    ])");
    ScenarioReport rep;
    std::string err;
    if (!run_loop_scenario(s, rep, err, r)) return false;
    if (rep.kpi.steps > s.max_steps + 1) {
        r.detail = "exceeded budget: " + std::to_string(rep.kpi.steps) +
                   " steps (max " + std::to_string(s.max_steps) + ")";
        return false;
    }
    r.detail = "stopped at " + std::to_string(rep.kpi.steps) +
               " steps (budget " + std::to_string(s.max_steps) + ")";
    return true;
}

// P-plan-adherence-chain: the model's stated plan (scripted as the first
// tool-call sequence) must be executed in dependency order — a read of the
// file that the write depends on must precede the write.
bool loop_probe_plan_adherence(ProbeResult& r) {
    r.expected = "plan steps execute in dependency order (read before write)";
    Scenario s;
    s.name = "probe-plan-adherence";
    s.prompt = "Read a.txt then update b.txt from it.";
    s.max_steps = 8;
    s.setup = agent::json::object({
        {"files",
         agent::json::object({{"a.txt", "needle"}, {"b.txt", "old"}})},
    });
    // The scripted plan: read a.txt, then write b.txt. The write's content
    // references the read's output, so order matters.
    s.fake_replies = agent::json::parse(R"([
        {"tool_calls": [{"id": "c1", "type": "function",
                         "function": {"name": "read",
                                      "arguments": "{\"path\":\"a.txt\"}"}}]},
        {"tool_calls": [{"id": "c2", "type": "function",
                         "function": {"name": "write",
                                      "arguments": "{\"path\":\"b.txt\",\"edits\":[{\"old\":\"old\",\"new\":\"needle\"}]}"}}]},
        {"content": "done"},
        {"content": "done"}
    ])");
    ScenarioReport rep;
    std::string err;
    if (!run_loop_scenario(s, rep, err, r)) return false;
    // The executed call order must be read-then-write.
    if (rep.tool_calls.size() != 2 ||
        rep.tool_calls[0].first.find("read") == std::string::npos ||
        rep.tool_calls[1].first.find("write") == std::string::npos) {
        r.detail = "execution order violated the plan dependency";
        return false;
    }
    r.detail = "read -> write executed in dependency order";
    return true;
}

// P-plan-design: the model's tool-call sequence is measured against the
// scenario's declared optimal plan (the plan-design contract). A scripted
// sequence matching the plan's tool mix must score full adherence.
bool loop_probe_plan_design(ProbeResult& r) {
    r.expected = "executed tool mix matches the declared optimal plan";
    Scenario s;
    s.name = "probe-plan-design";
    s.prompt = "Count the files then summarize.";
    s.max_steps = 8;
    s.setup = agent::json::object({
        {"files", agent::json::object({{"a.txt", "x"}, {"b.txt", "y"}})},
    });
    s.optimal_plan = agent::json::object({{"bash", 1}, {"read", 1}});
    s.oracle = {{"bash", {{"command", "*ls*"}}}, {"read", {{"path", "a.txt"}}}};
    s.fake_replies = agent::json::parse(R"([
        {"tool_calls": [{"id": "c1", "type": "function",
                         "function": {"name": "bash",
                                      "arguments": "{\"command\":\"ls\"}"}}]},
        {"tool_calls": [{"id": "c2", "type": "function",
                         "function": {"name": "read",
                                      "arguments": "{\"path\":\"a.txt\"}"}}]},
        {"content": "done"},
        {"content": "done"}
    ])");
    ScenarioReport rep;
    std::string err;
    if (!run_loop_scenario(s, rep, err, r)) return false;
    // The agentic plan machinery must record the declared plan and the
    // executed tool mix matching it.
    if (!rep.agentic.has_plan) {
        r.detail = "optimal_plan not recognized";
        return false;
    }
    if (rep.agentic.plan_ratio < 0.99) {
        r.detail = "plan ratio " + std::to_string(rep.agentic.plan_ratio) +
                   " (expected ~1.0 for a matching tool mix)";
        return false;
    }
    r.detail = "plan tool mix matched (ratio " +
               std::to_string(rep.agentic.plan_ratio) + ")";
    return true;
}

// P-plan-replan-adapt: after a tool failure, the model's next call must be
// able to differ (adaptation) — the harness must not force a repeat or
// hard-stop the loop. A failing call followed by a successful different one
// must complete and be flagged as an adaptation.
bool loop_probe_replan_adapt(ProbeResult& r) {
    r.expected = "a failure can be followed by a different call (adaptation)";
    Scenario s;
    s.name = "probe-replan-adapt";
    s.prompt = "Read a.txt, retry with b.txt if needed.";
    s.max_steps = 8;
    s.setup = agent::json::object({
        {"files", agent::json::object({{"b.txt", "needle"}})},
    });
    // First call fails (a.txt missing); the next call adapts to b.txt and
    // succeeds. The loop must survive the failure.
    s.fake_replies = agent::json::parse(R"([
        {"tool_calls": [{"id": "c1", "type": "function",
                         "function": {"name": "read",
                                      "arguments": "{\"path\":\"a.txt\"}"}}]},
        {"tool_calls": [{"id": "c2", "type": "function",
                         "function": {"name": "read",
                                      "arguments": "{\"path\":\"b.txt\"}"}}]},
        {"content": "done"},
        {"content": "done"}
    ])");
    ScenarioReport rep;
    std::string err;
    if (!run_loop_scenario(s, rep, err, r)) return false;
    if (rep.kpi.tool_failures == 0) {
        r.detail = "expected the first read to fail";
        return false;
    }
    if (rep.kpi.hard_stop) {
        r.detail = "hard-stopped after a single failure — no room to adapt";
        return false;
    }
    if (rep.kpi.steps < 3) {
        r.detail = "loop ended too early to allow adaptation";
        return false;
    }
    r.detail = "failed once, adapted to b.txt, completed";
    return true;
}

// P-dependency-order: an ordered oracle (write depends on read) must be
// violated when the model writes before reading — the violation must be
// measurable, not silently accepted.
bool loop_probe_dependency_order(ProbeResult& r) {
    r.expected = "write-before-read is measurable as a violation";
    Scenario s;
    s.name = "probe-dependency-order";
    s.prompt = "Update b.txt from a.txt.";
    s.max_steps = 8;
    s.setup = agent::json::object({
        {"files",
         agent::json::object({{"a.txt", "needle"}, {"b.txt", "old"}})},
    });
    // The oracle is ORDERED: read a.txt must precede the write of b.txt.
    s.oracle = {{"read", {{"path", "a.txt"}}},
                {"write", {{"path", "b.txt"}}}};
    s.fake_replies = agent::json::parse(R"([
        {"tool_calls": [{"id": "c1", "type": "function",
                         "function": {"name": "write",
                                      "arguments": "{\"path\":\"b.txt\",\"edits\":[{\"old\":\"old\",\"new\":\"needle\"}]}"}}]},
        {"tool_calls": [{"id": "c2", "type": "function",
                         "function": {"name": "read",
                                      "arguments": "{\"path\":\"a.txt\"}"}}]},
        {"content": "done"},
        {"content": "done"}
    ])");
    ScenarioReport rep;
    std::string err;
    if (!run_loop_scenario(s, rep, err, r)) return false;
    // The ordered oracle must flag the write-before-read.
    if (rep.kpi.bullseye >= 0.99) {
        r.detail = "ordered oracle accepted write-before-read";
        return false;
    }
    if (!rep.dependency_violation) {
        r.detail = "dependency violation not flagged";
        return false;
    }
    r.detail = "write-before-read flagged (bullseye " +
               std::to_string(rep.kpi.bullseye) + ")";
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

// P-budget-wall-clock: the wall-clock budget must be enforced — a run whose
// scripted replies take longer than max_wall_ms must be cut off.
bool budget_probe_wall_clock(ProbeResult& r) {
    r.expected = "run stops when the wall-clock budget is exhausted";
    Scenario s;
    s.name = "probe-budget-wall";
    s.suite = "harness";
    s.prompt = "Do this slowly forever.";
    s.max_steps = 100;
    s.max_wall_ms = 4000;
    // Each reply simulates 1.5s of latency; with 100 steps the run would
    // take minutes — the wall clock must cut it short.
    s.fake_replies = agent::json::array();
    for (int i = 0; i < 20; ++i) {
        agent::json tc;
        tc["id"] = "c1";
        tc["type"] = "function";
        tc["function"] = {{"name", "bash"},
                          {"arguments", {{"command", "true"}}}};
        s.fake_replies.push_back(
            agent::json::object({{"tool_calls", agent::json::array({tc})},
                                 {"latency_ms", 1500}}));
    }
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
    // The wall budget (4s) is far below the scripted total (~30s).
    if (rep.kpi.wall_ms > s.max_wall_ms + 2000) {
        r.detail = "wall budget ignored: " + std::to_string(rep.kpi.wall_ms) +
                   "ms vs budget " + std::to_string(s.max_wall_ms) + "ms";
        return false;
    }
    if (rep.kpi.steps >= 10) {
        r.detail = "ran too long: " + std::to_string(rep.kpi.steps) +
                   " steps past the wall budget";
        return false;
    }
    r.detail = "cut off at " + std::to_string(rep.kpi.wall_ms) +
               "ms / " + std::to_string(rep.kpi.steps) + " steps";
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

// P-dispatch-parallel: multiple calls in one message must all execute and
// each result must pair with the call that produced it (no cross-pairing).
bool dispatch_probe_parallel(ProbeResult& r) {
    r.expected = "two parallel calls execute; results pair to their calls";
    agent::Config cfg;
    cfg.mode = agent::AgentMode::Yolo;
    agent::ToolRegistry reg;
    reg.register_tool(agent::make_bash_tool());
    agent::ConversationLog log;
    std::set<std::string> approved;
    agent::Context dctx;

    agent::json calls = agent::json::array();
    {
        agent::json tc;
        tc["id"] = "cA";
        tc["type"] = "function";
        tc["function"] = {{"name", "bash"},
                          {"arguments", {{"command", "echo AAA"}}}};
        calls.push_back(tc);
    }
    {
        agent::json tc;
        tc["id"] = "cB";
        tc["type"] = "function";
        tc["function"] = {{"name", "bash"},
                          {"arguments", {{"command", "echo BBB"}}}};
        calls.push_back(tc);
    }

    // Capture each call's command -> output mapping (the result hook passes
    // the tool name, not the call id, so key by the command we sent).
    std::map<std::string, std::string> paired;
    agent::AgentHooks hooks;
    hooks.on_tool_call = [&paired](const std::string&,
                                   const agent::json& args) {
        paired[args.value("command", "")] = "";
    };
    hooks.on_tool_result =
        [&paired](const std::string&, const agent::ToolResult& res,
                  const agent::json&) {
            // Pair by the pending call's command, matched via output content.
            if (res.output.find("AAA") != std::string::npos)
                paired["echo AAA"] = res.output;
            if (res.output.find("BBB") != std::string::npos)
                paired["echo BBB"] = res.output;
        };
    const bool ok = agent::dispatch_tool_calls(calls, cfg, reg, hooks, log,
                                               approved, nullptr, &dctx);
    if (!ok) {
        r.detail = "dispatch returned false";
        return false;
    }
    if (paired["echo AAA"].find("AAA") == std::string::npos ||
        paired["echo BBB"].find("BBB") == std::string::npos) {
        r.detail = "results cross-paired or lost: A='" + paired["echo AAA"] +
                   "' B='" + paired["echo BBB"] + "'";
        return false;
    }
    r.detail = "two calls executed, results paired correctly";
    return true;
}

// P-dispatch-out-of-order: a tool that completes slowly must still pair its
// result with the right call when a sibling finishes first.
bool dispatch_probe_out_of_order(ProbeResult& r) {
    r.expected = "slow call's result pairs to its own id despite racing";
    agent::Config cfg;
    cfg.mode = agent::AgentMode::Yolo;
    agent::ToolRegistry reg;
    reg.register_tool(agent::make_bash_tool());
    agent::ConversationLog log;
    std::set<std::string> approved;
    agent::Context dctx;

    agent::json calls = agent::json::array();
    {
        agent::json tc;
        tc["id"] = "cSlow";
        tc["type"] = "function";
        tc["function"] = {{"name", "bash"},
                          {"arguments", {{"command", "sleep 0.3; echo SLOW"}}}};
        calls.push_back(tc);
    }
    {
        agent::json tc;
        tc["id"] = "cFast";
        tc["type"] = "function";
        tc["function"] = {{"name", "bash"},
                          {"arguments", {{"command", "echo FAST"}}}};
        calls.push_back(tc);
    }

    std::map<std::string, std::string> paired;
    agent::AgentHooks hooks;
    hooks.on_tool_call = [&paired](const std::string&,
                                   const agent::json& args) {
        paired[args.value("command", "")] = "";
    };
    hooks.on_tool_result =
        [&paired](const std::string&, const agent::ToolResult& res,
                  const agent::json&) {
            if (res.output.find("SLOW") != std::string::npos)
                paired["sleep 0.3; echo SLOW"] = res.output;
            if (res.output.find("FAST") != std::string::npos)
                paired["echo FAST"] = res.output;
        };
    const bool ok = agent::dispatch_tool_calls(calls, cfg, reg, hooks, log,
                                               approved, nullptr, &dctx);
    if (!ok) {
        r.detail = "dispatch returned false";
        return false;
    }
    if (paired["sleep 0.3; echo SLOW"].find("SLOW") == std::string::npos ||
        paired["echo FAST"].find("FAST") == std::string::npos) {
        r.detail = "out-of-order results cross-paired";
        return false;
    }
    r.detail = "slow + fast results paired to their own calls";
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

// P-recovery-nonretryable: a non-retryable error must NOT loop forever —
// the run must end (as a failure), not spin on retries.
bool recovery_probe_nonretryable(ProbeResult& r) {
    r.expected = "non-retryable error stops the run, no retry spin";
    Scenario s;
    s.name = "probe-recovery-nonretry";
    s.suite = "harness";
    s.prompt = "Do the thing.";
    s.max_steps = 10;
    s.max_wall_ms = 30000;
    // The reply throws a 401 (non-retryable); the run must not keep retrying.
    s.fake_replies = agent::json::array({
        agent::json::object({{"error", "auth failed"}, {"retryable", false}}),
        agent::json::object({{"content", "done"}}),
    });
    RunOptions opts;
    RunMeta meta;
    meta.mode = "hermetic";
    meta.model = "fake";
    std::string err;
    ScenarioReport rep = run_one_scenario(s, opts, meta, err);
    // The run may end in failure — but it must END (steps bounded, no spin).
    if (rep.kpi.retries >= 5) {
        r.detail = "non-retryable error was retried " +
                   std::to_string(rep.kpi.retries) + " times";
        return false;
    }
    if (rep.kpi.steps >= 8) {
        r.detail = "run spun: " + std::to_string(rep.kpi.steps) + " steps";
        return false;
    }
    r.detail = "run ended at " + std::to_string(rep.kpi.steps) +
               " steps, retries " + std::to_string(rep.kpi.retries);
    return true;
}

// P-recovery-dropout: a mid-stream dropout (server dies mid-reply) must be
// retried and the run must complete.
bool recovery_probe_dropout(ProbeResult& r) {
    r.expected = "mid-stream dropout is retried; run completes";
    Scenario s;
    s.name = "probe-recovery-dropout";
    s.suite = "harness";
    s.prompt = "Tell me about files.";
    s.max_steps = 5;
    s.max_wall_ms = 30000;
    s.stream = true;
    s.fake_replies = agent::json::array({
        agent::json::object({{"content", "part"},
                             {"drop_after_chunks", 2},
                             {"retryable", true}}),
        agent::json::object({{"content", "done"}}),
    });
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
    if (rep.kpi.hard_stop) {
        r.detail = "hard-stopped after a dropout";
        return false;
    }
    if (rep.kpi.retries == 0) {
        r.detail = "dropout was not retried";
        return false;
    }
    r.detail = "recovered after " + std::to_string(rep.kpi.retries) +
               " retries, completed";
    return true;
}

// P-recovery-4xx: a request-side 4xx the engine can repair (template-parser
// rejection -> drop tools, retry) must be repaired and the run must
// complete — not a dead end.
bool recovery_probe_4xx(ProbeResult& r) {
    r.expected = "template-parser 4xx is repaired (tools dropped) and retried";
    Scenario s;
    s.name = "probe-recovery-4xx";
    s.suite = "harness";
    s.prompt = "List the files.";
    s.max_steps = 5;
    s.max_wall_ms = 30000;
    // The server rejects the tool grammar ("Unable to generate parser for
    // this template"); the repair drops tools and retries.
    s.fake_replies = agent::json::array({
        agent::json::object({{"error",
                              "Unable to generate parser for this template"},
                             {"retryable", false}}),
        agent::json::object({{"content", "done"}}),
    });
    RunOptions opts;
    RunMeta meta;
    meta.mode = "hermetic";
    meta.model = "fake";
    std::string err;
    ScenarioReport rep = run_one_scenario(s, opts, meta, err);
    if (rep.kpi.recoveries == 0) {
        r.detail = "4xx produced no repair (recoveries=0)";
        return false;
    }
    r.detail = "4xx repaired and retried (recoveries " +
               std::to_string(rep.kpi.recoveries) + ")";
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// fidelity — tool-call correctness: wrong tool, wrong params, unknown tool,
// malformed args, wire shapes
// ---------------------------------------------------------------------------

namespace {

// P-misuse-wrong-tool: a scripted task that needs `read` but the model calls
// `bash cat` instead. The harness must dispatch it faithfully but the oracle
// must not match — misuse is measurable, not silently ignored.
bool fidelity_probe_misuse_wrong_tool(ProbeResult& r) {
    r.expected = "wrong-tool call dispatches but never matches the oracle";
    Scenario s;
    s.name = "probe-fidelity-misuse";
    s.suite = "harness";
    s.prompt = "What is in a.txt?";
    s.setup = agent::json::object({
        {"files", agent::json::object({{"a.txt", "needle"}})},
    });
    // The oracle expects `read a.txt`; the model calls `bash cat a.txt`.
    s.oracle = {{"read", {{"path", "a.txt"}}}};
    s.fake_replies = agent::json::parse(R"([
        {"tool_calls": [{"id": "c1", "type": "function",
                         "function": {"name": "bash",
                                      "arguments": "{\"command\":\"cat a.txt\"}"}}]},
        {"content": "done"},
        {"content": "done"}
    ])");
    ScenarioReport rep;
    std::string err;
    if (!run_loop_scenario(s, rep, err, r)) return false;
    if (rep.kpi.tool_calls != 1) {
        r.detail = "expected 1 executed call, got " +
                   std::to_string(rep.kpi.tool_calls);
        return false;
    }
    // The oracle step (read) must NOT be matched by the bash call.
    if (rep.kpi.bullseye >= 0.5) {
        r.detail = "wrong-tool call wrongly matched the read oracle";
        return false;
    }
    r.detail = "bash dispatched; read oracle not matched (bullseye " +
               std::to_string(rep.kpi.bullseye) + ")";
    return true;
}

// P-params-value-fidelity: the tool must receive exactly the argument
// values the model specified — value-level, not just key-level.
bool fidelity_probe_params_value(ProbeResult& r) {
    r.expected = "tool receives the exact argument values the model sent";
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
                      {"arguments",
                       {{"command", "echo hello-world-42"}}}};
    calls.push_back(tc);

    agent::AgentHooks hooks;
    std::string got_args;
    hooks.on_tool_call = [&got_args](const std::string&,
                                     const agent::json& args) {
        got_args = args.dump();
    };
    const bool ok = agent::dispatch_tool_calls(calls, cfg, reg, hooks, log,
                                               approved, nullptr, &dctx);
    if (!ok) {
        r.detail = "dispatch returned false";
        return false;
    }
    if (got_args.find("hello-world-42") == std::string::npos) {
        r.detail = "argument value lost: " + got_args;
        return false;
    }
    r.detail = "command value round-tripped: " + got_args;
    return true;
}

// P-unknown-tool: a tool_calls message naming a tool the registry does not
// provide must produce a graceful denial, not a crash or a stuck loop.
bool fidelity_probe_unknown_tool(ProbeResult& r) {
    r.expected = "unknown tool name denies gracefully, loop continues";
    Scenario s;
    s.name = "probe-fidelity-unknown";
    s.suite = "harness";
    s.prompt = "Call the mystery tool.";
    s.fake_replies = agent::json::parse(R"([
        {"tool_calls": [{"id": "c1", "type": "function",
                         "function": {"name": "nonexistent_tool",
                                      "arguments": "{}"}}]},
        {"content": "done"},
        {"content": "done"}
    ])");
    ScenarioReport rep;
    std::string err;
    if (!run_loop_scenario(s, rep, err, r)) return false;
    if (rep.kpi.hard_stop) {
        r.detail = "unknown tool caused a hard stop";
        return false;
    }
    r.detail = "unknown tool denied; loop continued (steps " +
               std::to_string(rep.kpi.steps) + ")";
    return true;
}

// P-malformed-args-repair: the engine must repair (or cleanly reject)
// malformed tool-call arguments without crashing or looping forever.
bool fidelity_probe_malformed_args(ProbeResult& r) {
    r.expected = "malformed arguments are handled without a crash";
    Scenario s;
    s.name = "probe-fidelity-malformed";
    s.suite = "harness";
    s.prompt = "Do the thing.";
    // arguments is not valid JSON — the classic truncated-args failure.
    s.fake_replies = agent::json::parse(R"([
        {"tool_calls": [{"id": "c1", "type": "function",
                         "function": {"name": "bash",
                                      "arguments": "{\"command\": \"ech"}}]},
        {"content": "done"},
        {"content": "done"}
    ])");
    ScenarioReport rep;
    std::string err;
    if (!run_loop_scenario(s, rep, err, r)) return false;
    if (rep.kpi.hard_stop) {
        r.detail = "malformed args caused a hard stop";
        return false;
    }
    r.detail = "malformed args handled; loop continued (steps " +
               std::to_string(rep.kpi.steps) + ")";
    return true;
}

// P-arg-object-vs-string: tool-call arguments arrive as a JSON object in
// some servers and as a JSON string in others; both must dispatch.
bool fidelity_probe_arg_shapes(ProbeResult& r) {
    r.expected = "arguments as object AND as string both dispatch";
    agent::Config cfg;
    cfg.mode = agent::AgentMode::Yolo;
    agent::ToolRegistry reg;
    reg.register_tool(agent::make_bash_tool());
    agent::ConversationLog log;
    std::set<std::string> approved;

    int dispatched = 0;
    agent::AgentHooks hooks;
    hooks.on_tool_call = [&dispatched](const std::string&,
                                       const agent::json&) { ++dispatched; };

    // Object-typed arguments.
    {
        agent::Context dctx;
        agent::json calls = agent::json::array();
        agent::json tc;
        tc["id"] = "c1";
        tc["type"] = "function";
        tc["function"] = {{"name", "bash"},
                          {"arguments", {{"command", "echo a"}}}};
        calls.push_back(tc);
        if (!agent::dispatch_tool_calls(calls, cfg, reg, hooks, log, approved,
                                        nullptr, &dctx)) {
            r.detail = "object-typed arguments failed";
            return false;
        }
    }
    // String-typed arguments.
    {
        agent::Context dctx;
        agent::json calls = agent::json::array();
        agent::json tc;
        tc["id"] = "c2";
        tc["type"] = "function";
        tc["function"] = {{"name", "bash"},
                          {"arguments", R"({"command":"echo b"})"}};
        calls.push_back(tc);
        if (!agent::dispatch_tool_calls(calls, cfg, reg, hooks, log, approved,
                                        nullptr, &dctx)) {
            r.detail = "string-typed arguments failed";
            return false;
        }
    }
    if (dispatched != 2) {
        r.detail = "expected 2 dispatches, got " +
                   std::to_string(dispatched);
        return false;
    }
    r.detail = "object and string argument shapes both dispatched";
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// output — tool-output interpretation: acting on content, truncation,
// envelope fidelity
// ---------------------------------------------------------------------------

namespace {

// P-output-acts-on-content: a scripted read returns content; the next tool
// call's arguments must reflect that content (the interpretation chain).
bool output_probe_acts_on_content(ProbeResult& r) {
    r.expected = "next call's args reflect the previous tool output";
    Scenario s;
    s.name = "probe-output-acts";
    s.suite = "harness";
    s.prompt = "Read a.txt then echo its content.";
    s.max_steps = 8;
    s.setup = agent::json::object({
        {"files", agent::json::object({{"a.txt", "needle"}})},
    });
    // read a.txt, then bash echo with the value the read returned.
    s.fake_replies = agent::json::parse(R"([
        {"tool_calls": [{"id": "c1", "type": "function",
                         "function": {"name": "read",
                                      "arguments": "{\"path\":\"a.txt\"}"}}]},
        {"tool_calls": [{"id": "c2", "type": "function",
                         "function": {"name": "bash",
                                      "arguments": "{\"command\":\"echo needle\"}"}}]},
        {"content": "done"},
        {"content": "done"}
    ])");
    ScenarioReport rep;
    std::string err;
    if (!run_loop_scenario(s, rep, err, r)) return false;
    if (rep.tool_calls.size() != 2) {
        r.detail = "expected 2 executed calls, got " +
                   std::to_string(rep.tool_calls.size());
        return false;
    }
    // The second call must reference the content read in the first.
    const auto& second = rep.tool_calls[1].second;
    if (second.find("needle") == std::string::npos) {
        r.detail = "second call does not reference the read content: " +
                   second;
        return false;
    }
    r.detail = "read output fed the next call";
    return true;
}

// P-output-truncation: large tool output must be capped, not dumped whole.
bool output_probe_truncation(ProbeResult& r) {
    r.expected = "oversized tool output is capped without a crash";
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
                      {"arguments",
                       {{"command", "head -c 200000 /dev/zero | tr '\\0' 'x'"}}}};
    calls.push_back(tc);

    agent::AgentHooks hooks;
    agent::ToolResult captured;
    hooks.on_tool_result = [&captured](const std::string&,
                                       const agent::ToolResult& res,
                                       const agent::json&) {
        captured = res;
    };
    const bool ok = agent::dispatch_tool_calls(calls, cfg, reg, hooks, log,
                                               approved, nullptr, &dctx);
    if (!ok) {
        r.detail = "dispatch returned false";
        return false;
    }
    // The cap is 64 KiB of tool output plus a short truncation marker.
    if (captured.output.size() > (64 * 1024) + 128) {
        r.detail = "output not capped: " +
                   std::to_string(captured.output.size()) + " bytes";
        return false;
    }
    if (captured.output.find("[output truncated") == std::string::npos) {
        r.detail = "no truncation marker: " +
                   std::to_string(captured.output.size()) + " bytes";
        return false;
    }
    r.detail = "output capped at " +
               std::to_string(captured.output.size()) + " bytes";
    return true;
}

// P-output-envelope-ext: the tool envelope must preserve the error text and
// meta fields on failure (the model needs the failure story, not a stub).
bool output_probe_envelope_ext(ProbeResult& r) {
    r.expected = "error text and meta survive the envelope";
    agent::ToolResult res;
    res.ok = false;
    res.error = "file not found: /nope";
    res.meta = {{"code", 2}};
    const std::string env =
        format_tool_envelope("read", {{"path", "/nope"}}, res);
    if (env.find("ERROR: file not found: /nope") == std::string::npos) {
        r.detail = "error text lost: " + env;
        return false;
    }
    if (env.find("status=error") == std::string::npos) {
        r.detail = "status lost: " + env;
        return false;
    }
    if (env.find("\"code\":2") == std::string::npos) {
        r.detail = "meta lost: " + env;
        return false;
    }
    r.detail = "error + status + meta preserved";
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
        add("context", "context_compression_rebuild",
            context_probe_compression_rebuild);
        add("context", "context_token_fidelity",
            context_probe_token_fidelity);
        add("envelope", "envelope_status_classification",
            envelope_probe_status_classification);
        add("budget", "budget_max_steps_enforced", budget_probe_max_steps_enforced);
        add("budget", "budget_wall_clock", budget_probe_wall_clock);
        add("loop", "loop_done_flag", loop_probe_done_flag);
        add("loop", "loop_continue_flag", loop_probe_continue_flag);
        add("loop", "loop_infinite_breakout", loop_probe_infinite_breakout);
        add("loop", "loop_text_repeat", loop_probe_text_repeat);
        add("loop", "loop_fail_streak", loop_probe_fail_streak);
        add("loop", "loop_no_false_positive", loop_probe_no_false_positive);
        add("loop", "loop_hard_stop_honesty", loop_probe_hard_stop_honesty);
        add("loop", "loop_plan_adherence", loop_probe_plan_adherence);
        add("loop", "loop_plan_design", loop_probe_plan_design);
        add("loop", "loop_replan_adapt", loop_probe_replan_adapt);
        add("loop", "loop_dependency_order", loop_probe_dependency_order);
        add("fidelity", "fidelity_misuse_wrong_tool",
            fidelity_probe_misuse_wrong_tool);
        add("fidelity", "fidelity_params_value", fidelity_probe_params_value);
        add("fidelity", "fidelity_unknown_tool", fidelity_probe_unknown_tool);
        add("fidelity", "fidelity_malformed_args", fidelity_probe_malformed_args);
        add("fidelity", "fidelity_arg_shapes", fidelity_probe_arg_shapes);
        add("output", "output_acts_on_content", output_probe_acts_on_content);
        add("output", "output_truncation", output_probe_truncation);
        add("output", "output_envelope_ext", output_probe_envelope_ext);
        add("confinement", "confinement_escapes_rejected",
            confinement_probe_escapes_rejected);
        add("oracle", "oracle_scenario_self_validation",
            oracle_probe_scenario_self_validation);
        add("dispatch", "dispatch_roundtrip", dispatch_probe_roundtrip);
        add("dispatch", "dispatch_parallel", dispatch_probe_parallel);
        add("dispatch", "dispatch_out_of_order", dispatch_probe_out_of_order);
        add("recovery", "recovery_retryable_recovers",
            recovery_probe_retryable_recovers);
        add("recovery", "recovery_nonretryable", recovery_probe_nonretryable);
        add("recovery", "recovery_dropout", recovery_probe_dropout);
        add("recovery", "recovery_4xx", recovery_probe_4xx);
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