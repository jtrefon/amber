
#ifndef AGENT_AGENT_HELPERS_H
#define AGENT_AGENT_HELPERS_H

#include <deque>
#include <string>
#include <vector>

#include "agent/llm.h" // Message, ToolResult, json
#include "agent/request_failure.h"
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
std::string empty_turn_reply(bool had_tool_calls, const std::string& last_failure);

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
//
// `budget` caps the payload: anything larger keeps its head and tail (UTF-8
// safe) around a marker reporting the omitted bytes, and `meta` gains `bytes`
// (the original size) and `truncated`. The envelope overhead is bounded, so a
// sealed tool message stays within roughly `budget` + 1 KiB.
inline constexpr std::size_t kToolResultBudgetDefault = std::size_t{32} * 1024;

std::string format_tool_envelope(const std::string& name, const json& args,
                                 const ToolResult& result,
                                 std::size_t budget = kToolResultBudgetDefault);

// The value of one call argument the envelope header echoes (path, pattern),
// or empty when the header omitted it. The header carries args only for compact
// calls, which is exactly the case a path lookup needs.
std::string tool_arg_of(const Message& message, const std::string& key);

// The `result_id` an envelope already carries, or empty when it has none. The
// id is what `read_result` needs, so a caller that is about to replace the
// payload asks first and stores it only when the answer is empty.
std::string result_id_of(const Message& message);

// Deterministic replacement for a bulky tool message pruned during
// compression: tool, status, original bytes and first/last line excerpts, so
// the model keeps an index instead of an opaque placeholder. `result_id` is
// used when the envelope did not carry one (the compactor stores the payload
// on demand); without either id the digest cannot promise recall.
std::string tool_result_digest(const Message& message, const std::string& result_id = {});

} // namespace agent

#endif // AGENT_AGENT_HELPERS_H
