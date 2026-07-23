// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AGENT_AGENT_HELPERS_H
#define AGENT_AGENT_HELPERS_H

#include <string>
#include <vector>

#include "agent/llm.h"  // Message, ToolResult, json
#include "agent/agent.h"  // AgentHooks

namespace agent { class MemoryStore; }

namespace agent {

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

// Extract XML-embedded tool calls from reply content (Jinja-style chat
// templates that emit XML instead of JSON tool_calls). On success it rewrites
// `tool_calls`/`content`/`stored` and returns true.
bool maybe_extract_text_tool_calls(json& tool_calls, std::string& content,
                                   Message& stored, const AgentHooks& hooks);

// One model round-trip that never aborts the turn: calls `chat`, sanitizes the
// reply text, and on any failure returns a recovered assistant error message so
// the loop can retry on the next iteration instead of crashing.
Message safe_chat_once(const AgentHooks& hooks, ConversationLog& log,
                       const std::function<Message()>& chat, const char* stage);

// Build the final-reply fallback when the loop ended without a usable answer.
std::string empty_turn_reply(const std::vector<Message>& history);

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
