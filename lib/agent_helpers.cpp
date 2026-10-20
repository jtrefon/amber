
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

std::string format_tool_envelope(const std::string& name, const json& args,
                                 const ToolResult& result) {
    // Ensure meta is always an object, never null (tools that return early
    // on error may leave meta uninitialized).
    json meta = result.meta.is_null() ? json::object() : result.meta;

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

    // Echo args only when compact: the model confirms what it sent (the
    // envelope's own contract), but large payloads (write edit blocks) only
    // bloat context and drive degradation.
    const std::string args_dump = args.dump();
    std::string header = "[tool=" + name + (args_dump.size() <= 120 ? " args=" + args_dump : "") +
                         " status=" + status + " meta=" + capped(meta.dump(), 300) + "]\n";

    std::string content = result.ok ? result.output : capped("ERROR: " + result.error, 2000);
    if (!content.empty() && content.back() != '\n')
        content += '\n';

    return header + content + "[end]";
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
    if (status == 429 || retry_after_ms > 0) {
        err.content = "[harness] the " + std::string(stage) +
                      " request was rate limited by the provider. It was not retried";
        if (retry_after_ms > 0)
            err.content += "; retry after " + std::to_string((retry_after_ms + 999) / 1000) + "s";
        err.content += ". Wait for the provider window before continuing.";
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

// A rate limit is a pause, not a retryable bug: one request, no ladder. The
// shared pacer already set its cooldown in the transport; retrying here is
// exactly the flood. Returns true with the stop notice when it applies.
bool stop_on_rate_limit(const char* stage, const LastFailure& last, const AgentHooks& hooks,
                        Message& out) {
    if (last.status != 429 && last.retry_after_ms <= 0)
        return false;
    if (hooks.on_status)
        hooks.on_status("provider rate limit; stopping instead of retrying");
    out = chat_error_reply(stage, last.error, last.attempt, last.status, last.retry_after_ms);
    return true;
}

enum class RetryDecision { Stop, Retry, Return };

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

// One failed attempt's policy: rate limit -> stop with a reply; a repairable
// non-retryable error -> repair and retry; otherwise the shared backoff decides.
RetryDecision handle_failure(RetryContext& ctx, const std::exception& e, int attempt,
                             int max_attempts, Message& out) {
    const auto* api = dynamic_cast<const ApiError*>(&e);
    ctx.last = {e.what(), retryable_error(e), attempt, api ? api->status : 0,
                api ? api->retry_after_ms : 0};
    ctx.log.event("chat_error",
                  {{"stage", ctx.stage}, {"error", ctx.last.error}, {"attempt", attempt}});
    if (ctx.hooks.on_debug)
        ctx.hooks.on_debug("chat error " + std::to_string(attempt) + ": " + ctx.last.error);
    if (stop_on_rate_limit(ctx.stage, ctx.last, ctx.hooks, out))
        return RetryDecision::Return;
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
            Message reply;
            const RetryDecision decision = handle_failure(ctx, e, attempt, max_attempts, reply);
            if (decision == RetryDecision::Return)
                return reply;
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
