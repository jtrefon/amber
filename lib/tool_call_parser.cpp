
#include "agent/tool_call_parser.h"

#include <nlohmann/json.hpp>

namespace agent {

namespace {

// Find the first occurrence of a tag, advancing `i` past the closing `>`.
// Returns true and sets `i` to the position after `>` on success.
bool skip_tag(const std::string& s, size_t& i, const std::string& tag) {
    auto pos = s.find("<" + tag + ">", i);
    if (pos == std::string::npos)
        return false;
    i = pos + tag.size() + 2; // past ">"
    return true;
}

// Read content between current `i` and the next matching closing tag.
// Advances `i` past the closing tag.
std::string read_until(const std::string& s, size_t& i, const std::string& close_tag) {
    auto end = s.find("</" + close_tag + ">", i);
    if (end == std::string::npos) {
        std::string rest = s.substr(i);
        i = s.size();
        return rest;
    }
    std::string content = s.substr(i, end - i);
    i = end + close_tag.size() + 3; // past "</name>"
    return content;
}

// Read an attribute-style tag "<tag=VALUE>" at or after `i`. On success
// advances `i` past the ">" and returns VALUE (trimmed). Returns empty
// without advancing when no such tag is found.
std::string read_attr_tag(const std::string& s, size_t& i, const std::string& tag) {
    auto pos = s.find("<" + tag + "=", i);
    if (pos == std::string::npos)
        return "";
    auto value_start = pos + tag.size() + 2; // past "="
    auto end = s.find('>', value_start);
    if (end == std::string::npos)
        return "";
    std::string v = s.substr(value_start, end - value_start);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t'))
        v.pop_back();
    i = end + 1;
    return v;
}

// Attempt to parse a tool call from a block that looks like:
//   {"name":"X","arguments":{"key":"val"}}
//   or just the inner content of a <tool_call> block.
json parse_json_tool_call(const std::string& block) {
    auto j = json::parse(block, nullptr, false);
    if (j.is_discarded())
        return {};
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
    if (!fn.contains("name"))
        return {};
    return tc;
}

// Scan `text` for the next top-level {...} block that parses as a
// Hermes-style tool call (bare JSON, no XML wrapper). String contents are
// respected so nested braces inside argument values don't confuse the
// depth count. On success fills `out` and returns true; `i` advances past
// the block either way (to text.size() when unbalanced).
bool scan_bare_json_call(const std::string& text, size_t& i, json& out) {
    size_t start = text.find('{', i);
    if (start == std::string::npos) {
        i = text.size();
        return false;
    }
    size_t j = start + 1;
    int depth = 1;
    bool in_str = false;
    for (; j < text.size() && depth > 0; ++j) {
        char c = text[j];
        if (in_str) {
            if (c == '\\')
                ++j;
            else if (c == '"')
                in_str = false;
        } else if (c == '"') {
            in_str = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
        }
    }
    i = (depth > 0) ? text.size() : j;
    if (depth > 0)
        return false; // unbalanced, no closing brace
    auto jc = parse_json_tool_call(text.substr(start, j - start));
    if (jc.is_null())
        return false;
    out = std::move(jc);
    return true;
}

// Pattern 1: <tool_call><name>X</name><arguments>...</arguments></tool_call>,
// or a JSON object inside the <tool_call> block. Common in Qwen/Jinja
// templates. Appends to `result`; reports whether anything was found.
bool extract_xml_tool_calls(const std::string& text, json& result) {
    bool found = false;
    size_t i = 0;
    while (i < text.size()) {
        size_t start = text.find("<tool_call>", i);
        if (start == std::string::npos)
            break;
        i = start + 11; // past "<tool_call>"

        json tc;
        tc["type"] = "function";
        auto& fn = tc["function"];

        // <name>X</name> + <arguments>JSON</arguments>
        size_t ni = i;
        if (skip_tag(text, ni, "name")) {
            fn["name"] = read_until(text, ni, "name");
            if (skip_tag(text, ni, "arguments")) {
                fn["arguments"] = read_until(text, ni, "arguments");
                i = ni; // advance past the close tag
                result.push_back(std::move(tc));
                found = true;
                continue;
            }
        }

        // Sub-pattern: the whole block is JSON.
        size_t close = text.find("</tool_call>", i);
        if (close == std::string::npos)
            break;
        std::string block = text.substr(i, close - i);
        i = close + 12; // past </tool_call>

        auto json_tc = parse_json_tool_call(block);
        if (!json_tc.is_null()) {
            result.push_back(std::move(json_tc));
            found = true;
        }
    }
    return found;
}

// Pattern 2: <function><name>X</name>...</function> (some legacy templates).
// The body is JSON, optionally wrapped as {"json": {...}}.
bool extract_function_blocks(const std::string& text, json& result) {
    bool found = false;
    size_t i = 0;
    while (i < text.size()) {
        size_t start = text.find("<function>", i);
        if (start == std::string::npos)
            break;
        i = start + 10; // past "<function>"
        size_t ni = i;
        if (!skip_tag(text, ni, "name"))
            break;
        std::string name = read_until(text, ni, "name");

        // Look for <parameter>, or just slurp the rest as JSON.
        size_t close = text.find("</function>", ni);
        std::string rest =
            (close != std::string::npos) ? text.substr(ni, close - ni) : text.substr(ni);
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
        found = true;
    }
    return found;
}

// Attribute-style parameter values are raw text; trim the surrounding
// whitespace (trailing first, then leading).
std::string trim_parameter_value(std::string value) {
    while (!value.empty() && (value.back() == ' ' || value.back() == '\n' || value.back() == '\r' ||
                              value.back() == '\t'))
        value.pop_back();
    const size_t lead = value.find_first_not_of(" \n\r\t");
    return lead == std::string::npos ? std::string() : value.substr(lead);
}

// Pattern 3: attribute-style calls (ornith template):
//   <tool_call>
//   <function=bash>
//   <parameter=command>
//   find . -type f
//   </parameter>
//   </function>
//   </tool_call>
// The parameter VALUE is the raw content until </parameter>; multiple
// <parameter> blocks merge into one arguments object. Unclosed blocks (the
// model cut off mid-call) still parse.
bool extract_attr_tool_calls(const std::string& text, json& result) {
    bool found = false;
    size_t i = 0;
    while (i < text.size()) {
        size_t start = text.find("<tool_call>", i);
        if (start == std::string::npos)
            break;
        i = start + 11; // past "<tool_call>"
        size_t ni = i;
        std::string fname = read_attr_tag(text, ni, "function");
        if (fname.empty())
            continue; // not the attribute style

        // Parameters belong to THIS call: never search past </function>.
        size_t fn_end = text.find("</function>", ni);
        if (fn_end == std::string::npos)
            fn_end = text.size();

        json args = json::object();
        size_t pi = ni;
        size_t param_pos = text.find("<parameter=", pi);
        while (param_pos != std::string::npos && param_pos < fn_end) {
            pi = param_pos;
            std::string key = read_attr_tag(text, pi, "parameter");
            args[key] = trim_parameter_value(read_until(text, pi, "parameter"));
            param_pos = text.find("<parameter=", pi);
        }

        json tc;
        tc["type"] = "function";
        tc["function"] = {{"name", fname}, {"arguments", args}};
        result.push_back(std::move(tc));
        found = true;
        size_t close = text.find("</tool_call>", pi);
        i = (close == std::string::npos) ? text.size() : close + 12;
    }
    return found;
}

// Pattern 4: Hermes-style bare JSON tool calls (Qwen2.5-Coder via llama.cpp
// text-mode): {"name":"X","arguments":{...}} appears as plain content with no
// XML wrapper. Each block is scanned independently so multiple calls in one
// response are all extracted.
bool extract_bare_json_calls(const std::string& text, json& result) {
    bool found = false;
    size_t i = 0;
    while (i < text.size()) {
        json tc;
        if (!scan_bare_json_call(text, i, tc))
            continue;
        result.push_back(std::move(tc));
        found = true;
    }
    return found;
}

} // namespace

// Scan `text` for tool calls in any of the dialects below. The dialects are
// mutually exclusive in practice: the first pattern that finds anything wins,
// so a response is never double-parsed by a second one.
json extract_tool_calls_from_text(const std::string& text) {
    json result = json::array();
    const bool found =
        extract_xml_tool_calls(text, result) || extract_function_blocks(text, result) ||
        extract_attr_tool_calls(text, result) || extract_bare_json_calls(text, result);
    return found ? result : json();
}

} // namespace agent
