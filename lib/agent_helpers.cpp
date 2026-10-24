
#include "agent/agent_helpers.h"
#include "agent/tool_call_parser.h"
#include "agent/llm.h"
#include "agent/tool.h"
#include "agent/run_scope.h"

#include <cctype>
#include <functional>
#include <stdexcept>
#include <unistd.h>
#include "agent/json_read.h"

namespace agent {

std::string fingerprint_tool_calls(const json& calls) {
    if (calls.is_null() || !calls.is_array() || calls.empty())
        return {};
    std::string key;
    for (const auto& tc : calls) {
        std::string id, fn;
        json args;
        bool ok = true;
        parse_tool_call(tc, id, fn, args, ok);
        if (!key.empty())
            key += '|';
        key += fn;
        key += '|';
        key += args.dump();
    }
    return key;
}

std::string canonical_call_key(const json& calls) {
    if (calls.is_null() || !calls.is_array() || calls.empty())
        return {};
    static const char* const kTimingArgs[] = {"timeout", "timeout_s", "seconds", "wait", "wait_s"};
    std::string key;
    for (const auto& tc : calls) {
        std::string id, fn;
        json args;
        bool ok = true;
        parse_tool_call(tc, id, fn, args, ok);
        if (args.is_object())
            for (const char* arg : kTimingArgs)
                args.erase(arg);
        if (!key.empty())
            key += '|';
        key += fn;
        key += '|';
        key += args.dump();
    }
    return key;
}

namespace {

// A field the model reads: enough to act on, and a marker when it was cut, so
// a truncated value is never mistaken for the whole one. Tool output is left
// alone — a reader's content is the result — this is for the fields around it,
// where an error dump or a metadata blob is what makes context expensive.
std::string capped(std::string text, std::size_t limit) {
    if (text.size() <= limit)
        return text;
    const std::size_t cut = text.size() - limit;
    text.resize(limit);
    return text + "\n[... " + std::to_string(cut) + " more characters]";
}

} // namespace

namespace {

bool is_utf8_continuation(unsigned char c) {
    return (c & 0xC0) == 0x80;
}

// Budget a payload middle-out: both ends survive (headers first, final errors
// last), the cut lands on a code-point boundary, and `omitted` reports how
// many bytes went away. `budget == 0` means "no cap". `handle` (when set) names
// the read_result call that retrieves the full payload.
std::string truncate_middle(const std::string& text, std::size_t budget, std::size_t& omitted,
                            const std::string& handle) {
    omitted = 0;
    if (budget == 0 || text.size() <= budget)
        return text;
    std::size_t head_len = budget / 2;
    std::size_t tail_len = budget - head_len;
    while (head_len > 0 && is_utf8_continuation(static_cast<unsigned char>(text[head_len])))
        --head_len;
    std::size_t tail_start = text.size() - tail_len;
    while (tail_start < text.size() &&
           is_utf8_continuation(static_cast<unsigned char>(text[tail_start])))
        ++tail_start;
    omitted = text.size() - head_len - (text.size() - tail_start);
    std::string marker = "\n[... " + std::to_string(omitted) + " bytes omitted of " +
                         std::to_string(text.size()) + " total";
    if (!handle.empty())
        marker += "; full output: " + handle;
    marker += "]\n";
    return text.substr(0, head_len) + marker + text.substr(tail_start);
}

} // namespace

std::string format_tool_envelope(const std::string& name, const json& args,
                                 const ToolResult& result, std::size_t budget) {
    // Ensure meta is always an object, never null (tools that return early
    // on error may leave meta uninitialized).
    json meta = result.meta.is_null() ? json::object() : result.meta;
    if (!meta.is_object())
        meta = json::object();

    std::string status;
    if (result.ok) {
        status = "ok";
    } else if (read_bool(meta, "denied", false)) {
        status = "denied";
    } else if (read_bool(meta, "timeout", false)) {
        status = "timeout";
    } else {
        status = "error";
    }

    std::string content = result.ok ? result.output : capped("ERROR: " + result.error, 2000);
    meta["bytes"] = content.size();
    std::string handle;
    if (meta.contains("result_id") && meta["result_id"].is_string())
        handle = "read_result result_id=" + meta["result_id"].get<std::string>();
    std::size_t omitted = 0;
    content = truncate_middle(content, budget, omitted, handle);
    if (omitted > 0)
        meta["truncated"] = true;

    // Echo args only when compact: the model confirms what it sent (the
    // envelope's own contract), but large payloads (write edit blocks) only
    // bloat context and drive degradation.
    const std::string args_dump = args.dump();
    std::string header = "[tool=" + name + (args_dump.size() <= 120 ? " args=" + args_dump : "") +
                         " status=" + status + " meta=" + capped(meta.dump(), 300) + "]\n";
    if (!content.empty() && content.back() != '\n')
        content += '\n';

    return header + content + "[end]";
}

namespace {

std::string header_field(const std::string& content, const std::string& field,
                         const std::string& terminators) {
    const size_t start = content.find(field);
    if (start == std::string::npos)
        return {};
    const size_t from = start + field.size();
    const size_t end = content.find_first_of(terminators, from);
    return content.substr(from, (end == std::string::npos ? content.size() : end) - from);
}

std::string header_meta(const std::string& content) {
    return header_field(content, "meta=", "]");
}

std::string header_status(const std::string& content) {
    const std::string status = header_field(content, "status=", " \n]");
    return status.empty() ? std::string("unknown") : status;
}

std::string meta_number(const std::string& meta, const std::string& key) {
    const std::string pattern = "\"" + key + "\":";
    const size_t at = meta.find(pattern);
    if (at == std::string::npos)
        return {};
    size_t from = at + pattern.size();
    size_t to = from;
    while (to < meta.size() && std::isdigit(static_cast<unsigned char>(meta[to])))
        ++to;
    return meta.substr(from, to - from);
}

std::string meta_string(const std::string& meta, const std::string& key) {
    const std::string pattern = "\"" + key + "\":\"";
    const size_t at = meta.find(pattern);
    if (at == std::string::npos)
        return {};
    const size_t from = at + pattern.size();
    const size_t to = meta.find('"', from);
    return meta.substr(from, (to == std::string::npos ? meta.size() : to) - from);
}

std::string one_line(const std::string& text, std::size_t cap) {
    std::string out = text.substr(0, cap);
    for (char& c : out)
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
    return out;
}

// The payload after the header line, with the closing [end] removed.
std::string envelope_body(const std::string& content) {
    const size_t nl = content.find('\n');
    if (nl == std::string::npos)
        return {};
    std::string body = content.substr(nl + 1);
    if (body.size() >= 5 && body.compare(body.size() - 5, 5, "[end]") == 0)
        body.resize(body.size() - 5);
    return body;
}

std::string first_line(const std::string& body) {
    size_t start = 0;
    while (start < body.size() && body[start] == '\n')
        ++start;
    const size_t end = body.find('\n', start);
    return one_line(body.substr(start, (end == std::string::npos ? body.size() : end) - start), 60);
}

std::string last_line(const std::string& body) {
    size_t end = body.size();
    while (end > 0 && (body[end - 1] == '\n' || body[end - 1] == '\r'))
        --end;
    const size_t start = end == 0 ? std::string::npos : body.rfind('\n', end - 1);
    const size_t from = (start == std::string::npos) ? 0 : start + 1;
    return one_line(body.substr(from, end - from), 60);
}

} // namespace

std::string tool_result_digest(const Message& message) {
    const std::string meta = header_meta(message.content);
    std::string bytes = meta_number(meta, "bytes");
    if (bytes.empty())
        bytes = std::to_string(message.content.size());
    std::string out = "[tool output omitted: tool=" +
                      (message.name.empty() ? std::string("unknown") : message.name) +
                      " status=" + header_status(message.content) + " bytes=" + bytes;
    const std::string id = meta_string(meta, "result_id");
    if (!id.empty())
        out += " result_id=" + id;
    const std::string body = envelope_body(message.content);
    const std::string head = first_line(body);
    const std::string tail = last_line(body);
    if (!head.empty())
        out += " first=\"" + head + "\"";
    if (!tail.empty() && tail != head)
        out += " last=\"" + tail + "\"";
    out += "]";
    return out;
}

std::string strip_think(std::string s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (s.compare(i, 7, "<think>") == 0) {
            size_t close = s.find("</think>", i + 7);
            if (close == std::string::npos)
                break; // truncated; drop trailing
            i = close + 8;
            continue;
        }
        out += s[i++];
    }
    return out;
}

// A UTF-8 continuation byte is 10xxxxxx.
bool is_continuation(unsigned char c) {
    return (c & 0xC0) == 0x80;
}

// The sequence length a lead byte introduces, or 0 when it is not a valid lead
// byte at all.
int sequence_length(unsigned char c) {
    if ((c >> 5) == 0x6)
        return 2;
    if ((c >> 4) == 0xE)
        return 3;
    if ((c >> 3) == 0x1E)
        return 4;
    return 0;
}

// True when the n-1 continuation bytes after `i` are present and valid.
bool continuation_ok(const std::string& s, size_t i, int n) {
    for (int k = 1; k < n; ++k)
        if (i + k >= s.size() || !is_continuation(static_cast<unsigned char>(s[i + k])))
            return false;
    return true;
}

// The UTF-8 replacement character, for a byte that cannot start a sequence.
void append_replacement(std::string& out) {
    out += '\xEF';
    out += '\xBF';
    out += '\xBD';
}

std::string utf8_sanitize(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            out += c;
            ++i;
            continue;
        }
        const int n = sequence_length(c);
        if (n == 0 || !continuation_ok(s, i, n)) {
            append_replacement(out);
            ++i;
            continue;
        }
        out.append(s, i, n);
        i += n;
    }
    return out;
}

void parse_tool_call(const json& call, std::string& id, std::string& fn, json& args, bool& ok) {
    ok = true;
    auto str_or = [](const json& j, const char* k, const std::string& d) -> std::string {
        auto it = j.find(k);
        return (it != j.end() && it->is_string()) ? it->get<std::string>() : d;
    };
    json fnobj = call.contains("function") && call["function"].is_object() ? call["function"]
                                                                           : json::object();
    id = str_or(call, "id", "");
    fn = str_or(fnobj, "name", "");
    args = fnobj.contains("arguments") && !fnobj["arguments"].is_null() ? fnobj["arguments"]
                                                                        : json::object();
    if (args.is_string()) {
        try {
            args = json::parse(args.get<std::string>());
        } catch (...) {
            ok = false;
        }
    }
}

long retry_backoff_ms(int attempt, const std::string& seed) {
    static const int kBase[] = {1000, 2000, 4000, 8000};
    const int step = attempt < 1 ? 1 : attempt;
    const int base = kBase[(step - 1) < 3 ? (step - 1) : 3];
    const int spread = base / 4; // ±25%
    const std::size_t h = std::hash<std::string>{}(seed + "#" + std::to_string(step));
    return base - spread + static_cast<long>(h % static_cast<std::size_t>((2 * spread) + 1));
}

namespace {

bool retryable_error(const std::exception& e) {
    // Cancellation is never a retryable failure: the user asked to stop.
    if (dynamic_cast<const CancelledError*>(&e))
        return false;
    const auto* api = dynamic_cast<const ApiError*>(&e);
    return api == nullptr || api->retryable;
}
bool wait_cancellable(const CancellationToken& token, int ms) {
    for (int waited = 0; waited < ms; waited += 100) {
        if (run_cancelled(token))
            return true;
        usleep(100 * 1000);
    }
    return false;
}
} // namespace

RequestFailure classify_request_failure(const std::string& error_text) {
    if (error_text.find("Unable to generate parser for this template") != std::string::npos)
        return RequestFailure::TemplateParser;
    if (error_text.find("does not exist") != std::string::npos ||
        error_text.find("model not found") != std::string::npos ||
        error_text.find("unknown model") != std::string::npos)
        return RequestFailure::ModelName;
    // Auth rejections come back as HTTP 401 (bad/expired key) or 403
    // (forbidden); the gateway error text always leads with the status.
    if (error_text.find("HTTP 401") != std::string::npos ||
        error_text.find("HTTP 403") != std::string::npos)
        return RequestFailure::Auth;
    return RequestFailure::None;
}

namespace {

// Shared retry loop: retries retryable errors with backoff; on a
// non-retryable error, asks `adapt` for a repaired request and runs ONE
// adapted attempt with full retry semantics. When `strict` is set the last
// error is rethrown on exhaustion instead of being returned as a message.
// One attempt: the chat call and the UTF-8 sanitising of what it returns.
Message attempt_chat(std::function<Message()>& chat) {
    Message m = chat();
    m.content = utf8_sanitize(m.content);
    m.reasoning = utf8_sanitize(m.reasoning);
    return m;
}

// Hand the failure to the adapter, once per call. Returns true when it produced a
// repaired chat function, which the caller then uses instead.
bool try_repair(const AgentHooks& hooks, ConversationLog& log, const char* stage,
                const std::string& error, const ChatAdapter& adapt,
                std::function<Message()>& chat) {
    if (!adapt)
        return false;
    std::function<Message()> repaired = adapt(error);
    if (!repaired)
        return false;
    chat = std::move(repaired);
    if (hooks.on_status)
        hooks.on_status("LLM request repaired, retrying (" + std::string(stage) + ")");
    log.event("chat_recovery", {{"stage", stage}, {"error", error}});
    return true;
}

// Report the failure and wait out the backoff. Returns false when the caller
// should stop: attempts exhausted, a non-retryable error, or a cancellation.
bool wait_for_retry(const AgentHooks& hooks, const CancellationToken& cancel_token, int attempt,
                    int max_attempts, bool retryable, std::string& last_error) {
    if (attempt >= max_attempts || !retryable)
        return false;
    const long wait_ms = retry_backoff_ms(attempt, last_error);
    if (hooks.on_status)
        hooks.on_status("LLM error - retrying (" + std::to_string(attempt) + "/" +
                        std::to_string(max_attempts) + ") in " + std::to_string(wait_ms) + "ms");
    if (wait_cancellable(cancel_token, static_cast<int>(wait_ms))) {
        last_error = "cancelled by user";
        return false;
    }
    return true;
}

// What a non-strict caller gets instead of an exception: a [harness] notice the
// model can act on. It rides the user channel (a system message would be hoisted
// to the head of the request by the OpenAI dialect) and carries `harness = true`
// so the loop never mistakes it for the model's own closing reply.
Message chat_error_reply(const char* stage, const std::string& last_error, int attempts,
                         long status = 0, long retry_after_ms = 0) {
    Message err;
    err.role = "user";
    err.harness = true;
    err.failure_detail = last_error;
    err.failure_status = status;
    err.failure_retry_after_ms = retry_after_ms;
    if (status == 429 || status == 529 || retry_after_ms > 0) {
        err.content = "[harness] the " + std::string(stage) +
                      " request is still rate limited after " + std::to_string(attempts) +
                      " attempt" + (attempts == 1 ? "" : "s");
        if (retry_after_ms > 0)
            err.content +=
                " (the provider asked for " + std::to_string((retry_after_ms + 999) / 1000) + "s)";
        err.content += ". The governor will pace the next request. You can call wait or throttle, "
                       "or tell the user what is blocked.";
        return err;
    }
    err.content = "[harness] the " + std::string(stage) + " request failed after " +
                  std::to_string(attempts) + " attempt" + (attempts == 1 ? "" : "s") + ": " +
                  last_error +
                  ". Decide the next move: adjust the request, try another approach, or tell the "
                  "user what is blocked.";
    return err;
}

// What the escalation reports: the error text, whether retrying had any
// prospect (which decides if a caller may keep the work it already has), and the
// attempt that produced it — the count the notice names.
struct LastFailure {
    std::string error;
    bool retryable = false;
    int attempt = 0;
    long status = 0;
    long retry_after_ms = 0;
};

enum class RetryDecision { Stop, Retry };

struct RetryContext {
    const AgentHooks& hooks;
    ConversationLog& log;
    const char* stage;
    const CancellationToken& cancel_token;
    const ChatAdapter& adapt;
    std::function<Message()>& chat;
    bool adapted = false;
    LastFailure last;

    RetryContext(const AgentHooks& h, ConversationLog& l, const char* s, const CancellationToken& c,
                 const ChatAdapter& a, std::function<Message()>& ch)
        : hooks(h), log(l), stage(s), cancel_token(c), adapt(a), chat(ch) {}
};

bool is_rate_limit(const LastFailure& last) {
    return last.status == 429 || last.status == 529 || last.retry_after_ms > 0;
}

// A 429 is a pause, not a dead turn. The governor has already set the pause and
// interval from the provider's Retry-After; the next attempt waits in the
// transport and probes once. Only a wait beyond the turn's patience, or an
// exhausted ladder, leaves the notice for the model to act on with wait/throttle.
RetryDecision rate_limit_decision(const RetryContext& ctx, int attempt, int max_attempts) {
    constexpr long kMaxRateWaitMs = 120000;
    if (attempt >= max_attempts || ctx.last.retry_after_ms > kMaxRateWaitMs)
        return RetryDecision::Stop;
    if (ctx.hooks.on_status) {
        std::string waiting = "provider rate limited - waiting for the pause";
        if (ctx.last.retry_after_ms > 0)
            waiting +=
                " (retry after " + std::to_string((ctx.last.retry_after_ms + 999) / 1000) + "s)";
        waiting += " before retry " + std::to_string(attempt) + "/" + std::to_string(max_attempts);
        ctx.hooks.on_status(waiting);
    }
    return RetryDecision::Retry;
}

// One failed attempt's policy: rate limit -> stop with a reply; a repairable
// non-retryable error -> repair and retry; otherwise the shared backoff decides.
RetryDecision handle_failure(RetryContext& ctx, const std::exception& e, int attempt,
                             int max_attempts) {
    const auto* api = dynamic_cast<const ApiError*>(&e);
    ctx.last = {e.what(), retryable_error(e), attempt, api ? api->status : 0,
                api ? api->retry_after_ms : 0};
    ctx.log.event("chat_error",
                  {{"stage", ctx.stage}, {"error", ctx.last.error}, {"attempt", attempt}});
    if (ctx.hooks.on_debug)
        ctx.hooks.on_debug("chat error " + std::to_string(attempt) + ": " + ctx.last.error);
    if (is_rate_limit(ctx.last))
        return rate_limit_decision(ctx, attempt, max_attempts);
    if (!ctx.adapted && !ctx.last.retryable &&
        try_repair(ctx.hooks, ctx.log, ctx.stage, ctx.last.error, ctx.adapt, ctx.chat)) {
        ctx.adapted = true;
        return RetryDecision::Retry;
    }
    if (!wait_for_retry(ctx.hooks, ctx.cancel_token, attempt, max_attempts, ctx.last.retryable,
                        ctx.last.error))
        return RetryDecision::Stop;
    return RetryDecision::Retry;
}

Message chat_with_retry_impl(const AgentHooks& hooks, ConversationLog& log,
                             std::function<Message()> chat, const char* stage,
                             const CancellationToken& cancel_token, int max_attempts,
                             const ChatAdapter& adapt) {
    RetryContext ctx{hooks, log, stage, cancel_token, adapt, chat};
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        try {
            return attempt_chat(chat);
        } catch (const std::exception& e) {
            const RetryDecision decision = handle_failure(ctx, e, attempt, max_attempts);
            if (decision == RetryDecision::Stop)
                break;
        }
    }
    if (run_cancelled(cancel_token)) {
        // Cancellation must not fabricate an assistant message: the caller
        // (Agent::run) ends the turn cleanly instead.
        throw CancelledError("request cancelled by user");
    }
    return chat_error_reply(stage, ctx.last.error, ctx.last.attempt, ctx.last.status,
                            ctx.last.retry_after_ms);
}

} // namespace

Message chat_with_retry(const AgentHooks& hooks, ConversationLog& log,
                        const std::function<Message()>& chat, const char* stage,
                        const CancellationToken& cancel_token, int max_attempts,
                        const ChatAdapter& adapt) {
    return chat_with_retry_impl(hooks, log, chat, stage, cancel_token, max_attempts, adapt);
}

// The turn's own facts, not the retained history's: after a compaction the
// context still holds tool messages from earlier turns, and scanning those made
// every empty reply claim it happened "after tool calls". A failure the harness
// reported in this turn is named, so a provider outage is not reported as a
// silent model.
std::string empty_turn_reply(bool had_tool_calls, const std::string& last_failure) {
    std::string what = had_tool_calls ? "the model produced no usable reply after tool calls"
                                      : "the model produced no usable reply";
    if (!last_failure.empty()) {
        std::string detail = last_failure;
        constexpr std::size_t kMaxDetail = 240;
        if (detail.size() > kMaxDetail) {
            detail.resize(kMaxDetail - 3);
            detail += "...";
        }
        what += "; the last request failed: " + detail;
    }
    return "[agent stopped: " + what + "]";
}

std::string internal_failure_reply(const std::string& what) {
    return "[harness] a failure while processing the previous reply: " + what +
           ". The turn continues; adjust your approach or retry the step.";
}

} // namespace agent
