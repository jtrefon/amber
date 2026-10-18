
#ifndef AGENT_AGENT_HELPERS_H
#define AGENT_AGENT_HELPERS_H

#include <deque>
#include <string>
#include <vector>

#include "agent/llm.h"   // Message, ToolResult, json
#include "agent/agent.h" // AgentHooks

namespace agent {
class MemoryStore;
}

namespace agent {

// Build a canonical fingerprint for a set of tool calls. Uses the same
// parse_tool_call logic as dispatch so comparisons are consistent: arguments
// are normalized to parsed JSON, then dump()-ed for ordering/stability.
// Returns "name1|args1|name2|args2|..." or empty string for null/empty input.
std::string fingerprint_tool_calls(const json& calls);

// The same fingerprint with timing arguments dropped ("timeout", "timeout_s",
// "seconds", "wait", "wait_s"): raising the timeout is an escalation of an
// identical call, not a new one, and counting it as new is what made a 30-retry
// loop look like 30 different calls to the old guard.
std::string canonical_call_key(const json& calls);

// Replace invalid UTF-8 sequences with U+FFFD so model/tool text can never
// carry bytes that make nlohmann throw on json::dump (type_error.316). Tool
// output (grep/semantic search) and some servers' assistant content can
// include binary.
std::string utf8_sanitize(std::string s);

// Strip inline <think>...</think> reasoning from whole-string assistant content
// (non-streaming path). A model that emits reasoning inside the content field
// instead of a separate reasoning_content must have those tags removed so they
// never reach the UI or the next turn. Mirror of the streaming segmenter.
std::string strip_think(std::string s);

// Parse one OpenAI tool_call delta into its id/name/args triple. The arguments
// field may arrive as a JSON string (streaming fragments); on parse failure
// `ok` is cleared and `args` keeps the raw string so dispatch can report a
// clear, recoverable error instead of a silent `{}`.
void parse_tool_call(const json& call, std::string& id, std::string& fn, json& args, bool& ok);

// Classify a non-retryable HTTP 4xx: is the failure in OUR request, and can
// the request be repaired by adaptation?
enum class RequestFailure : std::uint8_t {
    None,           // not a known recoverable request fault
    ModelName,      // server rejected the model id (vLLM-style "does not exist")
    TemplateParser, // server cannot parse the tool grammar for the loaded
                    // template (llama.cpp "Unable to generate parser ...")
    Auth            // API key/token rejected (HTTP 401/403)
};

RequestFailure classify_request_failure(const std::string& error_text);

// One-shot request-repair hook for non-retryable 4xx. Given the error text,
// returns an alternative chat callable that fixes the REQUEST (drop tools,
// swap to a server-known model) or {} when no repair applies.
using ChatAdapter = std::function<std::function<Message()>(const std::string&)>;

// Chat with retry/backoff for transient failures: up to `max_attempts`
// attempts with 1s->2s exponential backoff, sleeping in 100 ms slices that
// poll `cancel_token` (a request aborts the wait). Retryable errors are
// ApiError{retryable=true} (429/5xx/timeouts) or plain transport exceptions;
// non-retryable ApiErrors (auth/misconfig 4xx) fail fast after one attempt —
// unless `adapt` repairs the REQUEST (see ChatAdapter): then one adapted
// attempt runs with full retry semantics. On exhaustion returns a [harness]
// notice naming the attempts so the model knows the request failed and can
// choose the next move; the conversation stays intact.
Message chat_with_retry(const AgentHooks& hooks, ConversationLog& log,
                        const std::function<Message()>& chat, const char* stage,
                        const CancellationToken& cancel_token, int max_attempts = 3,
                        const ChatAdapter& adapt = {});

// The retry ladder: 1s, 2s, 4s, 8s (capped), each with ±25% jitter derived from
// the failure text and the attempt. A metronome is the worst shape of traffic
// for an endpoint that is recovering (or throttling): the jitter spreads retries
// out, while deriving it from the failure keeps a given failure's retries
// reproducible.
long retry_backoff_ms(int attempt, const std::string& seed);

// Build the final-reply fallback when the loop ended without a usable answer.
std::string empty_turn_reply(const std::deque<Message>& history);

// Build the message the harness hands the model when folding a reply into the
// turn failed (dispatch, a tool envelope, a host callback): the model must know
// what broke so it can adapt, and the conversation must stay valid.
std::string internal_failure_reply(const std::string& what);

// Format a tool result into the standard immutable envelope. Every tool output
// follows the exact same form regardless of status:
//
//   [tool=<name> args=<json> status=<status> meta=<json>]
//   <content>
//   [end]
//
// `name` is the tool name, `args` is the compact JSON of call arguments,
// `status` is one of "ok"|"error"|"denied"|"timeout", and `meta` holds
// tool-specific metadata (lines, exit code, duration, etc.). The envelope
// is token 0 — the model sees it before any content.
std::string format_tool_envelope(const std::string& name, const json& args,
                                 const ToolResult& result);

} // namespace agent

#endif // AGENT_AGENT_HELPERS_H
