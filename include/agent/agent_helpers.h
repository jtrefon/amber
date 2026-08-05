
#ifndef AGENT_AGENT_HELPERS_H
#define AGENT_AGENT_HELPERS_H

#include <deque>
#include <string>
#include <vector>

#include "agent/llm.h"  // Message, ToolResult, json
#include "agent/agent.h"  // AgentHooks

namespace agent { class MemoryStore; }

namespace agent {

// Build a canonical fingerprint for a set of tool calls. Uses the same
// parse_tool_call logic as dispatch so comparisons are consistent: arguments
// are normalized to parsed JSON, then dump()-ed for ordering/stability.
// Returns "name1|args1|name2|args2|..." or empty string for null/empty input.
std::string fingerprint_tool_calls(const json& calls);

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
void parse_tool_call(const json& call, std::string& id, std::string& fn,
                     json& args, bool& ok);

// Extract XML-embedded tool calls from reply content or reasoning
// (Jinja-style chat templates that emit XML instead of JSON tool_calls).
// When `content` is empty, falls back to `stored.reasoning` — some models
// (Qwen/Jinja) emit <tool_call> XML in their thinking block and leave
// content blank. On success it rewrites `tool_calls`, clears the source
// field (`content` or `stored.reasoning`), clears `content`, and returns
// true.
// One model round-trip that never aborts the turn: calls `chat`, sanitizes the
// reply text, and on any failure returns a recovered assistant error message so
// the loop can retry on the next iteration instead of crashing.
Message safe_chat_once(const AgentHooks& hooks, ConversationLog& log,
                       const std::function<Message()>& chat, const char* stage);

// Classify a non-retryable HTTP 4xx: is the failure in OUR request, and can
// the request be repaired by adaptation?
enum class RequestFailure : std::uint8_t {
    None,           // not a known recoverable request fault
    ModelName,      // server rejected the model id (vLLM-style "does not exist")
    TemplateParser  // server cannot parse the tool grammar for the loaded
                    // template (llama.cpp "Unable to generate parser ...")
};

RequestFailure classify_request_failure(const std::string& error_text);

// One-shot request-repair hook for non-retryable 4xx. Given the error text,
// returns an alternative chat callable that fixes the REQUEST (drop tools,
// swap to a server-known model) or {} when no repair applies.
using ChatAdapter =
    std::function<std::function<Message()>(const std::string&)>;

// Chat with retry/backoff for transient failures: up to `max_attempts`
// attempts with 1s->2s exponential backoff, sleeping in 100 ms slices that
// poll `cancel_token` (a request aborts the wait). Retryable errors are
// ApiError{retryable=true} (429/5xx/timeouts) or plain transport exceptions;
// non-retryable ApiErrors (auth/misconfig 4xx) fail fast after one attempt —
// unless `adapt` repairs the REQUEST (see ChatAdapter): then one adapted
// attempt runs with full retry semantics. On exhaustion returns the standard
// "[error during ...]" message so the loop degrades gracefully and the
// conversation stays intact.
Message chat_with_retry(const AgentHooks& hooks, ConversationLog& log,
                        const std::function<Message()>& chat,
                        const char* stage,
                        const CancellationToken& cancel_token,
                        int max_attempts = 3,
                        const ChatAdapter& adapt = {});

// Strict variant used by internal exchanges (the confirmation probe): same
// retry/adaptation behavior, but rethrows the last error on exhaustion
// instead of returning a fake reply that would pollute the context.
Message chat_with_retry_strict(const AgentHooks& hooks, ConversationLog& log,
                               const std::function<Message()>& chat,
                               const char* stage,
                               const CancellationToken& cancel_token,
                               int max_attempts = 3,
                               const ChatAdapter& adapt = {});

// Build the final-reply fallback when the loop ended without a usable answer.
std::string empty_turn_reply(const std::deque<Message>& history);

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
