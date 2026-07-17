// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#ifndef AGENT_TOOL_CALL_PARSER_H
#define AGENT_TOOL_CALL_PARSER_H

#include <string>
#include <nlohmann/json.hpp>

namespace agent {

using json = nlohmann::json;

// Detect and extract tool calls embedded in the text content of an LLM
// response. Different models/servers format tool calls differently:
//
//   OpenAI / llama.cpp native (without --jinja):
//     JSON tool_calls[] in the SSE delta — handled by sse_parser.
//
//   Qwen/Jinja (llama.cpp --jinja):
//     <tool_call><name>bash</name><arguments>{...}</arguments></tool_call>
//
//   DeepSeek / some templates:
//     <tool_call>{"name":"bash","arguments":{...}}</tool_call>
//
// extract_tool_calls_from_text scans `text` for known XML wrapper patterns
// and returns a json array matching the standard tool_calls format, or a
// null json value if nothing was found.
json extract_tool_calls_from_text(const std::string& text);

} // namespace agent

#endif
