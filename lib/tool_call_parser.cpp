
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

// Read an attribute-style tag "<tag=VALUE>" at or after `i`. On success
// advances `i` past the ">" and returns VALUE (trimmed). Returns empty
// without advancing when no such tag is found.
std::string read_attr_tag(const std::string& s, size_t& i,
                          const std::string& tag) {
    auto pos = s.find("<" + tag + "=", i);
    if (pos == std::string::npos) return "";
    auto value_start = pos + tag.size() + 2;  // past "="
    auto end = s.find('>', value_start);
    if (end == std::string::npos) return "";
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

    // Pattern 3: attribute-style tool calls (ornith template):
    //   <tool_call>
    //   <function=bash>
    //   <parameter=command>
    //   find . -type f
    //   </parameter>
    //   </function>
    //   </tool_call>
    // The parameter VALUE is the raw content until </parameter>; multiple
    // <parameter> blocks merge into one arguments object. Unclosed blocks
    // (the model cut off mid-call) still parse.
    if (!found_any) {
        i = 0;
        while (i < text.size()) {
            size_t start = text.find("<tool_call>", i);
            if (start == std::string::npos) break;
            i = start + 11;  // past "<tool_call>"
            size_t ni = i;
            std::string fname = read_attr_tag(text, ni, "function");
            if (fname.empty()) continue;  // not the attribute style
            // Parameters belong to THIS call: never search past </function>.
            size_t fn_end = text.find("</function>", ni);
            if (fn_end == std::string::npos) fn_end = text.size();
            json args = json::object();
            size_t pi = ni;
            size_t param_pos = text.find("<parameter=", pi);
            while (param_pos != std::string::npos && param_pos < fn_end) {
                pi = param_pos;
                std::string key = read_attr_tag(text, pi, "parameter");
                std::string value = read_until(text, pi, "parameter");
                while (!value.empty() &&
                       (value.back() == ' ' || value.back() == '\n' ||
                        value.back() == '\r' || value.back() == '\t'))
                    value.pop_back();
                size_t lead = value.find_first_not_of(" \n\r\t");
                if (lead != std::string::npos)
                    value = value.substr(lead);
                else
                    value.clear();
                args[key] = value;
                param_pos = text.find("<parameter=", pi);
            }
            json tc;
            tc["type"] = "function";
            tc["function"] = {{"name", fname}, {"arguments", args}};
            result.push_back(std::move(tc));
            found_any = true;
            size_t close = text.find("</tool_call>", pi);
            i = (close == std::string::npos) ? text.size() : close + 12;
        }
    }

    return found_any ? result : json();
}

} // namespace agent
