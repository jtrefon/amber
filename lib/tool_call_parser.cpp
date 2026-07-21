// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Jacek Trefon (www.trefon.com)

#include "agent/tool_call_parser.h"

#include <nlohmann/json.hpp>

namespace agent {

namespace {

// Find the first occurrence of a tag, advancing `i` past the closing `>`.
// Returns true and sets `i` to the position after `>` on success.
bool skip_tag(const std::string& s, size_t& i, const std::string& tag) {
    auto pos = s.find("<" + tag + ">", i);
    if (pos == std::string::npos) return false;
    i = pos + tag.size() + 2;  // past ">"
    return true;
}

// Read content between current `i` and the next matching closing tag.
// Advances `i` past the closing tag.
std::string read_until(const std::string& s, size_t& i,
                       const std::string& close_tag) {
    auto end = s.find("</" + close_tag + ">", i);
    if (end == std::string::npos) {
        std::string rest = s.substr(i);
        i = s.size();
        return rest;
    }
    std::string content = s.substr(i, end - i);
    i = end + close_tag.size() + 3;  // past "</name>"
    return content;
}

// Attempt to parse a tool call from a block that looks like:
//   {"name":"X","arguments":{"key":"val"}}
//   or just the inner content of a <tool_call> block.
json parse_json_tool_call(const std::string& block) {
    auto j = json::parse(block, nullptr, false);
    if (j.is_discarded()) return {};
    json tc;
    tc["type"] = "function";
    auto& fn = tc["function"];
    if (j.contains("name") && j["name"].is_string())
        fn["name"] = j["name"].get<std::string>();
    if (j.contains("arguments")) {
        if (j["arguments"].is_string())
            fn["arguments"] = j["arguments"].get<std::string>();
        else if (j["arguments"].is_object())
            fn["arguments"] = j["arguments"].dump();
    }
    if (!fn.contains("name")) return {};
    return tc;
}

} // namespace

json extract_tool_calls_from_text(const std::string& text) {
    json result = json::array();
    size_t i = 0;
    bool found_any = false;

    // Pattern 1: <tool_call><name>X</name><arguments>...</arguments></tool_call>
    // Common in Qwen/Jinja templates.
    while (i < text.size()) {
        size_t start = text.find("<tool_call>", i);
        if (start == std::string::npos) break;
        i = start + 11;  // past "<tool_call>"

        // Look for two possible sub-structures:
        //   <name>X</name><arguments>JSON</arguments>
        //   or JSON inside the tool_call block directly.

        json tc;
        tc["type"] = "function";
        auto& fn = tc["function"];

        // Try <name>...</name> + <arguments>...</arguments>
        size_t ni = i;
        bool has_name = skip_tag(text, ni, "name");
        if (has_name) {
            std::string name = read_until(text, ni, "name");
            fn["name"] = name;
            has_name = skip_tag(text, ni, "arguments");
            if (has_name) {
                std::string args = read_until(text, ni, "arguments");
                fn["arguments"] = args;
                i = ni;  // advance past the close tag
                result.push_back(std::move(tc));
                found_any = true;
                continue;
            }
        }

        // Sub-pattern inside <tool_call>: parse the entire block as JSON
        //   <tool_call>{"name":"X","arguments":{...}}</tool_call>
        // Find the closing tag from current position.
        size_t close = text.find("</tool_call>", i);
        if (close == std::string::npos) break;
        std::string block = text.substr(i, close - i);
        i = close + 12;  // past </tool_call>

        auto json_tc = parse_json_tool_call(block);
        if (!json_tc.is_null()) {
            result.push_back(std::move(json_tc));
            found_any = true;
        }
    }

    // Pattern 2: <function><name>X</name>...</function> (some legacy templates)
    if (!found_any) {
        i = 0;
        while (i < text.size()) {
            size_t start = text.find("<function>", i);
            if (start == std::string::npos) break;
            i = start + 10;  // past "<function>"
            size_t ni = i;
            if (!skip_tag(text, ni, "name")) break;
            std::string name = read_until(text, ni, "name");
            // Look for <parameter> or just slurp the rest as JSON
            size_t close = text.find("</function>", ni);
            std::string rest = (close != std::string::npos)
                ? text.substr(ni, close - ni) : text.substr(ni);
            auto jrest = json::parse(rest, nullptr, false);
            i = (close != std::string::npos) ? close + 11 : text.size();

            json tc;
            tc["type"] = "function";
            auto& fn = tc["function"];
            fn["name"] = name;
            if (!jrest.is_discarded() && jrest.contains("json"))
                fn["arguments"] = jrest["json"].dump();
            else
                fn["arguments"] = rest;
            result.push_back(std::move(tc));
            found_any = true;
        }
    }

    return found_any ? result : json();
}

} // namespace agent
